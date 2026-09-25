/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * The WOPI-native content check.
 * Classes: ContentCheck
 */

#pragma once

#include <common/JsonUtil.hpp>
#include <common/StateEnum.hpp>

#include <Poco/JSON/Object.h>

#include <net/HttpRequest.hpp>

#include <cctype>
#include <string>
#include <string_view>

namespace DLP
{
/// The verdict of the WOPI-native content check.
///
/// A WOPI host that supports content checks is told so through the
/// "X-Vaulterix-Capabilities: content-check" header on CheckFileInfo, and
/// reports its verdict in a "VaulterixContentCheck" member of the response.
///
/// The host may not be able to decide right away (inspecting a large document
/// takes a while), in which case it answers State::Pending along with a
/// CheckId.  The CheckId is then polled through the content-check endpoint
/// until a final verdict is known.  The document is only loaded once the
/// verdict is State::Allowed; State::Blocked denies access, and
/// State::Unavailable means the check couldn't be completed (in which case
/// access is denied too, as the alternative would be to bypass the check).
class ContentCheck final
{
        STATE_ENUM(State, Unknown, Allowed, Pending, Blocked, Unavailable);

        static State parseState(const std::string_view state) noexcept
        {
            if (state == "ALLOWED")
            {
                return State::Allowed;
            }
            if (state == "PENDING")
            {
                return State::Pending;
            }
            if (state == "BLOCKED")
            {
                return State::Blocked;
            }
            if (state == "UNAVAILABLE")
            {
                return State::Unavailable;
            }

            return State::Unknown;
        }
public:
    static ContentCheck createUnavaliable(const std::string& checkId, const std::string& version)
    {
        return ContentCheck(State::Unavailable, checkId, version);
    }

    ContentCheck(std::string_view contentCheckHeader, const Poco::JSON::Object::Ptr& responseBody, http::StatusCode code)
        : _state(parseState(contentCheckHeader))
    {
        if (Poco::JSON::Object::Ptr contentCheck; responseBody && (contentCheck = responseBody->getObject("VaulterixContentCheck")))
        {
            _checkId = contentCheck->has("CheckId") ? contentCheck->getValue<std::string>("CheckId") : "";
            _version = contentCheck->has("Version") ? contentCheck->getValue<std::string>("Version") : "";
            if (_state == State::Unknown && contentCheck->has("State"))
            {
                _state = parseState(contentCheck->getValue<std::string>("State"));
            }
        }
        if (!isConsistentWith(code))
        {
            _state = State::Unknown;
        }
        if (_state == State::Unknown)
        {
            LOG_WRN("Unknown content-check state (header=[" << contentCheckHeader << "])");
        }
    }

    bool completed() const noexcept { return _state != State::Pending; }
    bool isAllowed() const noexcept { return _state == State::Allowed; }
    bool isPending() const noexcept { return _state == State::Pending; }
    bool isBlocked() const noexcept { return _state == State::Blocked; }
    bool isUnavailable() const noexcept { return _state == State::Unavailable; }
    bool isUnknown() const noexcept { return _state == State::Unknown; }
    /// Whether a pending verdict can still be resolved by polling.
    bool canPoll() const noexcept { return isPending() && !_checkId.empty(); }

    /// The identifier of the pending check, to be polled.
    /// Note that a pending check without a CheckId can never be resolved.
    const std::string& checkId() const noexcept { return _checkId; }
    /// The version of the content check, as reported by the host.
    const std::string& version() const noexcept { return _version; }
    std::string_view errorKind() const noexcept
    {
        switch (_state)
        {
        case State::Blocked:     return "contentcheckblocked";
        case State::Unavailable: return "contentcheckunavailable";
        default:                 return "";
        }
    }
    std::string message() const noexcept
    {
        switch (_state)
        {
        case State::Blocked:     return std::format(
            "DLP prohibited opening the document, checkId: [{}], content check version: [{}]", _checkId, _version);
        case State::Unavailable: return std::format(
            "DLP service is unavailable, checkId: [{}], content check version: [{}]", _checkId, _version);
        default:                 return "";
        }
    }

    std::string stateStr() noexcept
    {
        switch (_state)
        {
        case State::Allowed:     return "ALLOWED";
        case State::Pending:     return "PENDING";
        case State::Blocked:     return "BLOCKED";
        case State::Unavailable: return "UNAVAILABLE";
        default:                 return "UNKNOWN";
        }
    }

private:
    ContentCheck(State state, const std::string& checkId, const std::string& version) noexcept
        : _checkId(checkId)
        , _version(version)
        , _state(state)
    {}

    bool isConsistentWith(http::StatusCode code) const noexcept
    {
        switch (code)
        {
            case http::StatusCode::OK:
                return isAllowed() || (isPending() && !_checkId.empty());
            case http::StatusCode::Forbidden:
                return isBlocked();
            case http::StatusCode::ServiceUnavailable:
                return isUnavailable();
            default:
                return true;
        }
    }

private:
    /// Identifier of the pending check, when the state is Pending.
    std::string _checkId;
    /// Version of the content check, as reported by the host.
    std::string _version;
    State _state;
};
} // namespace
/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
