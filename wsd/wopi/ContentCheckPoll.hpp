/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * Copyright the Collabora Online contributors.
 *
 * SPDX-License-Identifier: MPL-2.0
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/*
 * Polling of the WOPI content-check endpoint.
 * Classes: ContentCheckPoll
 */

#pragma once

#if MOBILEAPP
#error This file should be excluded from Mobile App builds
#endif // MOBILEAPP

#include <common/ConfigUtil.hpp>
#include <net/HttpRequest.hpp>
#include <net/Socket.hpp>
#include <wopi/ContentCheck.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <Poco/URI.h>

namespace DLP
{
/// Asynchronously polls the WOPI content-check endpoint until the host has a
/// final verdict, or we give up.
///
/// CheckFileInfo can answer with a pending content check, in which case the
/// host is still inspecting the document and the verdict has to be awaited.
/// Inspecting a large document can take a long time, which is why we poll
/// asynchronously on the vetting TerminatingPoll (websrv_poll), instead of blocking
/// it, and why the station waits for the verdict before creating a DocBroker.
/// That way a pending check doesn't pin a Kit process, a DocBroker slot, or a
/// thread of the WebServer.
///
/// The endpoint is derived from the CheckFileInfo URL (therefore it carries the
/// same access_token) by appending "/content-check/<CheckId>" to its path:
///
///   GET {WOPISrc}/content-check/{CheckId}?access_token=...
///
/// The host answers with the same "VaulterixContentCheck" object it used in
/// CheckFileInfo, plus a "Retry-After" header to pace the polling, if it wants.
/// A 403 is a denial, while 404/410 mean the check is gone (and we can't know
/// the verdict, so we deny too).  Anything else transient (5xx, timeouts,
/// connection failures) is retried until the deadline, after which the verdict
/// becomes State::Unavailable.
class ContentCheckPoll final : public std::enable_shared_from_this<ContentCheckPoll>
{
public:
    using FinishedCallback = std::function<void(ContentCheckPoll&)>;

    ContentCheckPoll(const std::shared_ptr<TerminatingPoll>& poll, const Poco::URI& wopiSrc, const ContentCheck& check, FinishedCallback onFinished);

    bool start();

    /// Stop polling.  The finished callback will not be invoked anymore.
    void cancel() noexcept
    {
        _cancelled = true;
        ++_generation;
    }

    const ContentCheck& check() const noexcept { return _check; }
    unsigned attempts() const noexcept { return _attempts; }

    std::string getSslVerifyMessage() const
    {
        return _httpSession ? _httpSession->getSslVerifyMessage() : std::string();
    }

private:
    /// Issue one poll request. Returns false when we're done.
    bool poll();

    void scheduleNext(std::chrono::milliseconds delay);

    void handleResponse(const http::Response& response, std::chrono::milliseconds elapsed);

    void finish(ContentCheck::State state, std::string message);

    /// The URI of the content-check endpoint for our CheckId.
    Poco::URI makeEndpointUri() const;

    /// The delay before the next poll, given the last round trip.
    std::chrono::milliseconds nextPollDelay(std::chrono::milliseconds retryAfter,
                                            std::chrono::milliseconds elapsed) const;

private:
    std::shared_ptr<TerminatingPoll> _poll;
    Poco::URI _wopiSrc;
    ContentCheck _check;
    FinishedCallback _onFinished;
    std::shared_ptr<http::Session> _httpSession;
    const std::chrono::milliseconds _pollIntervalMs;
    /// The maximum duration of the whole check.
    const std::chrono::seconds _timeout;
    const unsigned _maxAttempts;
    /// The deadline of the whole check, set when starting.
    std::chrono::steady_clock::time_point _deadline;
    /// The number of requests already issued.
    std::atomic<unsigned> _attempts = 0;
    /// Bumped to invalidate continuations we scheduled but no longer want.
    unsigned _generation = 0;
    /// Set when we shouldn't poll anymore.
    std::atomic<bool> _cancelled = false;
};
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
