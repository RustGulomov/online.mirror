/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
#pragma once

#if MOBILEAPP
#error This file should be excluded from Mobile App builds
#endif // MOBILEAPP

#include <common/ConfigUtil.hpp>
#include <net/HttpRequest.hpp>
#include <net/Socket.hpp>
#include <wopi/ContentCheck.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <Poco/URI.h>

namespace DLP
{
/// Asynchronously polls the WOPI content-check endpoint until the host returns
/// a final verdict, or we give up. Polling never blocks the poll it runs on.
///
/// The endpoint is derived from the document URL (so it carries the same
/// access_token) by appending the check id to its path:
///
///   GET {WOPISrc}/content-check/{CheckId}?access_token=...
///
/// The host answers with the same "VaulterixContentCheck" object it used in
/// CheckFileInfo.
///
/// With an operation (e.g. "print") the check starts at the collection URI,
/// with the operation in the "X-Vaulterix-Operation" header; the host either
/// answers right away, or hands us a fresh CheckId to poll as above:
///
///   POST {WOPISrc}/content-check?access_token=...   X-Vaulterix-Operation: print
///
/// The host reports the LastModifiedTime of the version it checked (see
/// lastModifiedTime()), so the caller can refuse to act on a version that is no
/// longer the saved one.
class ContentCheckPoll : public std::enable_shared_from_this<ContentCheckPoll>
{
public:
    using FinishedCallback = std::function<void(ContentCheckPoll&)>;

    ContentCheckPoll(const std::shared_ptr<TerminatingPoll>& poll, const Poco::URI& wopiSrc,
                     const ContentCheck& check, FinishedCallback onFinished,
                     const std::string& operation = std::string());

    bool start();
    /// Stop polling. The finished callback will not be invoked anymore.
    void cancel() noexcept;

    const ContentCheck& check() const noexcept;
    unsigned attempts() const noexcept;
    /// The LastModifiedTime the host reported for the checked version, if any.
    const std::string& lastModifiedTime() const noexcept;

private:
    /// Issue one poll request. Returns false when we're done.
    bool poll();

    void scheduleNext(std::chrono::milliseconds delay);

    void handleResponse(const http::Response& response, std::chrono::milliseconds elapsed);

    void finish();

private:
    std::shared_ptr<TerminatingPoll> _poll;
    /// The WOPI URI of the document, without the content-check path.
    Poco::URI _baseUri;
    /// The URI of the current request: the collection URI for an operation
    /// check, or the {base}/content-check/{CheckId} endpoint we are polling.
    Poco::URI _wopiSrc;
    ContentCheck _check;
    /// The operation to verify, when the check is for an operation rather than
    /// for loading the document (e.g. "print").  Empty for the load-time check.
    const std::string _operation;
    /// The LastModifiedTime of the checked version, as reported by the host.
    std::string _lastModifiedTime;
    FinishedCallback _onFinished;
    std::shared_ptr<http::Session> _httpSession;
    const std::chrono::milliseconds _pollIntervalMs;
    /// The maximum duration of the whole check.
    const std::chrono::seconds _timeout;
    /// The deadline of the whole check, set when starting.
    std::chrono::steady_clock::time_point _deadline;
    /// The number of requests already issued.
    std::atomic<unsigned> _attempts = 0;
    /// Set when we shouldn't poll anymore.
    std::atomic<bool> _cancelled = false;
    bool _started = false;
};
}

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
