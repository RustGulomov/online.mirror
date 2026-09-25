/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * Implementation of the WOPI content-check polling.
 * Classes: ContentCheckPoll
 */

#include <config.h>

#if MOBILEAPP
#error "Mobile doesn't need or support WOPI"
#endif

#include "ContentCheckPoll.hpp"

#include <common/Anonymizer.hpp>
#include <common/ConfigUtil.hpp>
#include <common/Log.hpp>
#include <common/Protocol.hpp>
#include <common/SigUtil.hpp>
#include <common/Util.hpp>
#include <net/Uri.hpp>
#include <wopi/StorageConnectionManager.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include <Poco/URI.h>

namespace
{
static constexpr std::chrono::milliseconds MaxRetryAfterMs{30000};

/// The longest a single poll request may take.
static constexpr std::chrono::seconds MaxRequestTimeout{30};

static constexpr std::string_view EndpointPrefix = "/content-check/";

Poco::URI makeEndpointUri(const Poco::URI& wopiSrc, const std::string& checkId)
{
    Poco::URI uri(wopiSrc);

    std::string path = uri.getPath();
    while (!path.empty() && path.back() == '/')
    {
        path.pop_back();
    }

    path.append(EndpointPrefix);
    path.append(Uri::encode(checkId));
    uri.setPath(path);
    return uri;
}

} // namespace

using namespace DLP;

ContentCheckPoll::ContentCheckPoll(const std::shared_ptr<TerminatingPoll>& poll,
                                   const Poco::URI& wopiSrc, const ContentCheck& check,
                                   FinishedCallback onFinished)
    : _poll(poll)
    , _wopiSrc(::makeEndpointUri(wopiSrc, check.checkId()))
    , _check(check)
    , _onFinished(std::move(onFinished))
    , _pollIntervalMs(std::chrono::seconds(1))
    , _timeout(std::chrono::minutes(2))
    , _maxAttempts(120)
{
    LOG_INF("ContentCheck: polling checkId ["
            << _check.checkId() << "] of [" << Anonymizer::anonymizeUrl(_wopiSrc.toString())
            << "] every " << _pollIntervalMs.count() << "ms for up to " << _timeout.count()
            << "s (" << _maxAttempts << " attempts max)");
}

bool ContentCheckPoll::start()
{
    LOG_ASSERT(_poll && "Must have a SocketPoll");
    LOG_ASSERT(_check.isPending() && "Can only poll a pending content check");

    if (_cancelled || !(_check.isPending()))
    {
        return false;
    }

    _deadline = std::chrono::steady_clock::now() + _timeout;

    return poll();
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

        finish(ContentCheck::State::Unavailable, _check.message().empty() ? "the content check didn't complete in time" : _check.message());
        return false;
    }

    if (_attempts >= _maxAttempts)
    {
        finish(ContentCheck::State::Unavailable, _check.message().empty() ? "the content check exceeded the maximum number of attempts" : _check.message());
        return false;
    }

    ++_attempts;

    const Poco::URI uri = makeEndpointUri();
    const std::string uriAnonym = Anonymizer::anonymizeUrl(uri.toString());
    LOG_DBG("ContentCheck: poll [" << _attempts << '/' << _maxAttempts << "] checkId ["
                                   << _check.checkId() << "] on [" << uriAnonym << ']');

    // Bound every request, so that a hung host doesn't hold us indefinitely.
    const std::chrono::seconds remaining =
        std::chrono::duration_cast<std::chrono::seconds>(_deadline - now) + std::chrono::seconds(1);
    _httpSession = StorageConnectionManager::getHttpSession(uri, std::min(remaining, MaxRequestTimeout));

    const Authorization auth = Authorization::create(uri);
    const http::Request httpRequest = StorageConnectionManager::createHttpRequest(uri, auth);

    const auto startTime = std::chrono::steady_clock::now();

    _httpSession->setFinishedHandler(
        [selfWeak = weak_from_this(), this, startTime](
            const std::shared_ptr<http::Session>& session)
        {
            // Keep ourselves alive for the duration of the callback.
            const std::shared_ptr<ContentCheckPoll> selfLifecycle = selfWeak.lock();
            if (!selfLifecycle || _cancelled)
                return;

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
                return;

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

        finish(ContentCheck::State::Unavailable, "failed to issue a content-check request");
        return false;
    }

    return true;
}

void ContentCheckPoll::handleResponse(const http::Response& response,
                                      const std::chrono::milliseconds elapsed)
{
    const http::StatusCode statusCode = response.statusLine().statusCode();
    const std::string& body = response.getBody();

    const std::string uriAnonym = Anonymizer::anonymizeUrl(makeEndpointUri().toString());

    LOG_DBG("ContentCheck: poll [" << _attempts << '/' << _maxAttempts << "] checkId ["
                                   << _check.checkId() << "] returned "
                                   << static_cast<unsigned>(statusCode) << " in "
                                   << elapsed.count() << "ms on [" << uriAnonym << ']');

    std::string message;
    bool pendingInBody = false;

    _check = ContentCheck(code, body);

    if (statusCode == http::StatusCode::Forbidden)
    {
        finish(ContentCheck::State::Blocked, std::move(message));
        return;
    }

    Poco::JSON::Object::Ptr json;
    if (!body.empty() && JsonUtil::parseJSON(body, json))
    {
        const ContentCheck verdict = ContentCheck::parse(json);
        switch (verdict.state())
        {
            case ContentCheck::State::Allowed:
                if (statusCode == http::StatusCode::OK)
                {
                    _check = verdict;
                    finish(ContentCheck::State::Allowed, verdict.message());
                    return;
                }

                LOG_WRN("ContentCheck: checkId ["
                        << _check.checkId() << "] was approved with HTTP status "
                        << static_cast<unsigned>(statusCode) << "; ignoring the approval");
                break;

            case ContentCheck::State::Blocked:
                _check = verdict;
                finish(ContentCheck::State::Blocked, verdict.message());
                return;

            case ContentCheck::State::Unavailable:
                _check = verdict;
                finish(ContentCheck::State::Unavailable, verdict.message());
                return;

            case ContentCheck::State::Pending:
                pendingInBody = true;
                message = verdict.message();
                break;

            case ContentCheck::State::Unknown:
            default:
                break;
        }
    }

    if (statusCode == http::StatusCode::Forbidden)
    {
        finish(ContentCheck::State::Blocked, std::move(message));
        return;
    }

    if (statusCode == http::StatusCode::NotFound || statusCode == http::StatusCode::Gone)
    {
        // The check is unknown (or was dropped); the verdict can no longer be
        // determined, and we don't want to fall back to loading the document.
        finish(ContentCheck::State::Unavailable,
               "the content check is no longer known to the host");
        return;
    }

    if (!pendingInBody && !(statusCode == http::StatusCode::OK) && statusCode != http::StatusCode::Accepted)
    {
        // A transient failure on the host side (5xx, 429, timeouts, ...).
        // Log it and try again; the deadline and the attempt limit bound us.
        LOG_WRN("ContentCheck: poll ["
                << _attempts << "] for checkId [" << _check.checkId() << "] failed with "
                << static_cast<unsigned>(statusCode) << " on [" << uriAnonym << "]; will retry. "
                << "Body: [" << COOLProtocol::getAbbreviatedMessage(body) << ']');
    }

    // Remember what the host says, so that we can tell the user what we were
    // waiting for, should we give up.
    if (!message.empty())
        _check = ContentCheck(ContentCheck::State::Pending, _check.checkId(), std::move(message), _check.version());

    scheduleNext(nextPollDelay(retryAfter, elapsed));
}

std::chrono::milliseconds
ContentCheckPoll::nextPollDelay(const std::chrono::milliseconds retryAfter,
                               const std::chrono::milliseconds elapsed) const
{
    // Prefer the pacing the host asked for.  Otherwise don't poll more often
    // than the configured interval, but count the time the host already spent
    // holding the request towards it (so a host that keeps the request open
    // for a long time doesn't make us wait for the full interval on top).
    if (retryAfter > std::chrono::milliseconds::zero())
        return std::min(retryAfter, MaxRetryAfterMs);

    return elapsed < _pollIntervalMs ? _pollIntervalMs - elapsed : std::chrono::milliseconds::zero();
}

void ContentCheckPoll::scheduleNext(const std::chrono::milliseconds delay)
{
    if (_cancelled || _check.completed())
        return;

    if (delay <= std::chrono::milliseconds::zero())
    {
        // The host already took long enough; go again right away.
        poll();
        return;
    }

    // SocketPoll has no timer API (addCallback always fires immediately), so a
    // short-lived helper thread does the waiting and posts the continuation to
    // the poll thread.  Only one continuation is ever outstanding, and the
    // generation counter invalidates the ones we no longer want.
    const unsigned generation = ++_generation;
    LOG_TRC("ContentCheck: polling checkId [" << _check.checkId() << "] again in " << delay.count() << "ms");

    std::thread([selfWeak = weak_from_this(), poll = _poll, generation, delay]()
        {
            std::this_thread::sleep_for(delay);

            if (!selfWeak.lock())
            {
                return;
            }

            poll->addCallback([selfWeak, generation]()
                {
                    if (const std::shared_ptr<ContentCheckPoll> self = selfWeak.lock())
                    {
                        if (!self->_cancelled && self->_generation == generation)
                        {
                            self->poll();
                        }
                    }
                });
        })
        .detach();
}

void ContentCheckPoll::finish(const ContentCheck::State state, std::string message)
{
    LOG_ASSERT(ContentCheck::completed(state) && "Must finish with a final verdict");

    if (_cancelled)
    {
        return;
    }

    _check = ContentCheck(state, _check.checkId(), std::move(message), _check.version());

    ++_generation; // Any scheduled continuation is stale now.

    LOG_INF("ContentCheck: checkId [" << _check.checkId() << "] of ["
                                      << Anonymizer::anonymizeUrl(_wopiSrc.toString()) << "] is "
                                      << ContentCheck::name(state) << " after " << _attempts
                                      << " poll(s)"
                                      << (_check.message().empty() ? std::string()
                                                                   : ": " + _check.message()));

    FinishedCallback onFinished = std::move(_onFinished);
    _onFinished = nullptr;
    if (onFinished)
    {
        onFinished(*this);
    }
}
