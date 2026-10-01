/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */

#include <config.h>

#if MOBILEAPP
#error "Mobile doesn't need or support WOPI"
#endif

#include "ContentCheckPoll.hpp"

#include <common/Anonymizer.hpp>
#include <common/ConfigUtil.hpp>
#include <common/JsonUtil.hpp>
#include <common/Log.hpp>
#include <common/Protocol.hpp>
#include <common/SigUtil.hpp>
#include <common/Util.hpp>
#include <wopi/StorageConnectionManager.hpp>

#include <chrono>
#include <string>
#include <thread>

#include <Poco/URI.h>

namespace
{
static constexpr std::chrono::seconds MaxRequestTimeout{30};
static constexpr unsigned             MaxAttempts{120};
static constexpr std::string_view     EndpointPrefix{"/content-check"};

Poco::URI makeEndpointUri(const Poco::URI& wopiSrc, const std::string& checkId)
{
    Poco::URI uri(wopiSrc);

    std::string path = uri.getPath();
    while (!path.empty() && path.back() == '/')
    {
        path.pop_back();
    }

    path.append(EndpointPrefix);
    if (!checkId.empty())
    {
        path.append("/");
        path.append(checkId);
    }

    uri.setPath(path);
    return uri;
}

} // namespace

using namespace DLP;

ContentCheckPoll::ContentCheckPoll(const std::shared_ptr<TerminatingPoll>& poll,
                                   const Poco::URI& wopiSrc, const ContentCheck& check,
                                   FinishedCallback onFinished, const std::string& operation)
    : _poll(poll)
    , _baseUri(wopiSrc)
    , _wopiSrc(::makeEndpointUri(wopiSrc, check.checkId()))
    , _check(check)
    , _operation(operation)
    , _onFinished(std::move(onFinished))
    , _pollIntervalMs(std::chrono::seconds(1))
    , _timeout(std::chrono::minutes(2))
{
    LOG_INF("ContentCheck: "
            << (_operation.empty() ? "polling" : _operation) << " checkId [" << _check.checkId()
            << "] of [" << Anonymizer::anonymizeUrl(_wopiSrc.toString()) << "] every "
            << _pollIntervalMs.count() << "ms for up to " << _timeout.count() << "s (" << MaxAttempts
            << " attempts max)");
}

bool ContentCheckPoll::start()
{
    LOG_ASSERT(_poll && "Must have a SocketPoll");
    LOG_ASSERT(_check.isPending() && "Can only poll a pending content check");

    if (_cancelled || !_check.isPending() || _started)
    {
        return false;
    }

    _started = true;
    _deadline = std::chrono::steady_clock::now() + _timeout;

    return poll();
}

void ContentCheckPoll::cancel() noexcept
{
    _cancelled = true;
}

const ContentCheck& ContentCheckPoll::check() const noexcept
{
    return _check;
}

unsigned ContentCheckPoll::attempts() const noexcept
{
    return _attempts;
}

const std::string& ContentCheckPoll::lastModifiedTime() const noexcept
{
    return _lastModifiedTime;
}

bool ContentCheckPoll::poll()
{
    if (_cancelled || _check.completed())
    {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= _deadline)
    {
        LOG_WRN("ContentCheck: deadline of " << _timeout.count() << "s exceeded after "
                << _attempts << " poll(s) for checkId [" << _check.checkId()
                << "] of [" << Anonymizer::anonymizeUrl(_wopiSrc.toString())
                << "]; giving up");

        _check = ContentCheck::createUnavaliable(_check.checkId(), _check.version());
        finish();
        return false;
    }

    if (_attempts >= MaxAttempts)
    {
        LOG_WRN("ContentCheck: reached the limit of " << MaxAttempts
                << " poll attempts for checkId [" << _check.checkId()
                << "] of [" << Anonymizer::anonymizeUrl(_wopiSrc.toString())
                << "]; giving up");

        _check = ContentCheck::createUnavaliable(_check.checkId(), _check.version());
        finish();
        return false;
    }

    ++_attempts;

    const std::string uriAnonym = Anonymizer::anonymizeUrl(_wopiSrc.toString());
    LOG_DBG("ContentCheck: poll [" << _attempts << '/' << MaxAttempts << "] checkId ["
                                   << _check.checkId() << "] on [" << uriAnonym << ']');

    const std::chrono::seconds remaining =
        std::chrono::duration_cast<std::chrono::seconds>(_deadline - now) + std::chrono::seconds(1);
    _httpSession = StorageConnectionManager::getHttpSession(_wopiSrc, std::min(remaining, MaxRequestTimeout));
    const Authorization auth = Authorization::create(_wopiSrc);
    http::Request httpRequest = StorageConnectionManager::createHttpRequest(_wopiSrc, auth);

    if (!_operation.empty() && _attempts == 1)
    {
        LOG_DBG("ContentCheck: requesting the " << _operation << " check on [" << uriAnonym << ']');
        httpRequest.setVerb(http::Request::VERB_POST);
        httpRequest.set("X-Vaulterix-Operation", _operation);
    }

    _httpSession->setFinishedHandler(
        [selfWeak = weak_from_this(), this, startTime = now](
            const std::shared_ptr<http::Session>& session)
        {
            const std::shared_ptr<ContentCheckPoll> selfLifecycle = selfWeak.lock();
            if (!selfLifecycle || _cancelled)
            {
                return;
            }

            if (SigUtil::getShutdownRequestFlag())
            {
                LOG_DBG("ContentCheck: shutdown flagged, giving up on the check");
                return;
            }

            const std::shared_ptr<const http::Response> httpResponse = session->response();
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - startTime);

            handleResponse(*httpResponse, elapsed);
        });

    _httpSession->setConnectFailHandler(
        [selfWeak = weak_from_this(), this](const std::shared_ptr<http::Session>& session)
        {
            const std::shared_ptr<ContentCheckPoll> selfLifecycle = selfWeak.lock();
            if (!selfLifecycle || _cancelled)
            {
                return;
            }

            LOG_WRN("ContentCheck: failed to connect to ["
                    << Anonymizer::anonymizeUrl(session ? session->host() : std::string())
                    << "] while polling checkId [" << _check.checkId() << "]; will retry");

            // Retry; the deadline and the attempt limit bound us.
            scheduleNext(_pollIntervalMs);
        });

    if (!_httpSession->asyncRequest(httpRequest, _poll))
    {
        LOG_ERR("ContentCheck: failed to issue a poll request for checkId ["
                << _check.checkId() << "] on [" << uriAnonym << ']');
        _check = ContentCheck::createUnavaliable(_check.checkId(), _check.version());
        finish();
        return false;
    }

    return true;
}

void ContentCheckPoll::handleResponse(const http::Response& response, std::chrono::milliseconds elapsed)
{
    const http::StatusCode statusCode = response.statusLine().statusCode();
    const std::string uriAnonym = Anonymizer::anonymizeUrl(_wopiSrc.toString());

    LOG_DBG("ContentCheck: poll [" << _attempts << '/' << MaxAttempts << "] checkId ["
                                   << _check.checkId() << "] returned "
                                   << static_cast<unsigned>(statusCode) << " in "
                                   << elapsed.count() << "ms on [" << uriAnonym << ']');

    Poco::JSON::Object::Ptr responseBody = nullptr;
    JsonUtil::parseJSON(response.getBody(), responseBody);

    if (responseBody)
        _lastModifiedTime = JsonUtil::getJSONValue<std::string>(responseBody, "LastModifiedTime");

    _check = ContentCheck(response.get("X-Vaulterix-Content-Check"), responseBody, statusCode);

    if (_check.isPending())
    {
        if (!_check.checkId().empty())
        {
            _wopiSrc = ::makeEndpointUri(_baseUri, _check.checkId());
        }

        scheduleNext(elapsed < _pollIntervalMs ? _pollIntervalMs - elapsed : std::chrono::milliseconds::zero());
    }
    else
    {
        finish();
    }
}

void ContentCheckPoll::scheduleNext(const std::chrono::milliseconds delay)
{
    if (_cancelled || _check.completed())
    {
        return;
    }

    if (delay <= std::chrono::milliseconds::zero())
    {
        _poll->addCallback([selfWeak = weak_from_this()]()
            {
                if (const std::shared_ptr<ContentCheckPoll> self = selfWeak.lock(); self && !self->_cancelled)
                {
                    self->poll();
                }
            });
        return;
    }

    // SocketPoll has no timer API (addCallback always fires immediately), so a
    // short-lived helper thread does the waiting and posts the continuation to
    // the poll thread.
    LOG_TRC("ContentCheck: polling checkId [" << _check.checkId() << "] again in " << delay.count() << "ms");

    std::thread([selfWeak = weak_from_this(), poll = _poll, delay]()
        {
            std::this_thread::sleep_for(delay);

            if (selfWeak.expired())
            {
                return;
            }

            poll->addCallback([selfWeak]()
                {
                    if (const std::shared_ptr<ContentCheckPoll> self = selfWeak.lock(); self && !self->_cancelled)
                    {
                        self->poll();
                    }
                });
        })
        .detach();
}

void ContentCheckPoll::finish()
{
    if (_cancelled)
    {
        return;
    }

    LOG_INF("ContentCheck: checkId [" << _check.checkId() << "] of ["
                                      << Anonymizer::anonymizeUrl(_wopiSrc.toString()) << "] is "
                                      << _check.stateStr() << " after " << _attempts
                                      << " poll(s)"
                                      << (_check.message().empty() ? std::string()
                                                                   : ": " + _check.message()));

    FinishedCallback onFinished;
    std::swap(onFinished, _onFinished);
    if (onFinished)
    {
        onFinished(*this);
    }
}
