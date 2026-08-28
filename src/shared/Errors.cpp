#include "Errors.h"
#include "Log.h"

#ifdef ENABLE_CPPTRACE
#include <cpptrace/cpptrace.hpp>
#include <thread>
#endif

#ifdef ENABLE_CPPTRACE
// Shared by the sync and async paths: only formats/logs an already-resolved trace, does
// no DWARF symbol resolution itself (that's the expensive part, callers do it up front).
static void LogResolvedStacktrace(cpptrace::stacktrace const& st)
{
    bool hasStacktraceInfo = false;
    for (size_t i = 0; i < st.frames.size(); ++i)
    {
        cpptrace::stacktrace_frame const& trace = st.frames[i];
        if (trace.line.has_value())
        {
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "#%zu [0x%" PRIXPTR "] %s %s:%u",
                i,
                trace.object_address,
                trace.symbol.c_str(),
                trace.filename.c_str(),
                trace.line.value()
            );
        }
        else
        {
            // without line
            sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL,
                "#%zu [0x%" PRIXPTR "] %s %s",
                i,
                trace.object_address,
                trace.symbol.c_str(),
                trace.filename.c_str()
            );
        }

        if (!hasStacktraceInfo && trace.line.has_value() && !trace.symbol.empty())
        {
            // we assume there are symbols if at least one frame was parsed successfully
            hasStacktraceInfo = true;
        }
    }

    if (!hasStacktraceInfo)
    {
        // without line
#if PLATFORM == PLATFORM_WINDOWS
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Missing debug symbols. Place an up-to-date PDB file next to executable and/or build with debug symbols.");
#else
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "Missing debug symbols. Please build with debug symbols.");
#endif
    }
}
#endif

void MaNGOS::Errors::PrintStacktrace()
{
    PrintStacktrace(1, 64);
}

void MaNGOS::Errors::PrintStacktrace(int skipFrames, int maxFrames)
{
#ifndef ENABLE_CPPTRACE
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "Stack traces using cpptrace are disabled. Set ENABLE_CPPTRACE=ON (default) during CMake configuration to enable them.");
#else
    cpptrace::stacktrace st = cpptrace::generate_trace(
        std::size_t(skipFrames) + 1, // we want to skip our own frame
        std::size_t(maxFrames)
    );
    LogResolvedStacktrace(st);
#endif
}

void MaNGOS::Errors::PrintStacktraceAsync()
{
    PrintStacktraceAsync(1, 64);
}

void MaNGOS::Errors::PrintStacktraceAsync(int skipFrames, int maxFrames)
{
#ifndef ENABLE_CPPTRACE
    sLog.Out(LOG_BASIC, LOG_LVL_MINIMAL, "Stack traces using cpptrace are disabled. Set ENABLE_CPPTRACE=ON (default) during CMake configuration to enable them.");
#else
    // Cheap: just unwinds and records raw frame addresses, no DWARF symbol lookup yet.
    cpptrace::raw_trace raw = cpptrace::generate_raw_trace(
        std::size_t(skipFrames) + 1, // we want to skip our own frame
        std::size_t(maxFrames)
    );

    // Expensive part (DWARF symbol resolution) happens off the calling thread, so a hot
    // path that hits this can never stall waiting for it - not even on the first call.
    // try/catch is required here and NOT redundant with the synchronous PrintStacktrace()
    // path having none: an exception escaping a detached std::thread's entry function
    // always calls std::terminate() (killing the whole process) regardless of whatever
    // try/catch the *caller* of PrintStacktraceAsync() has - there is no caller stack left
    // to unwind into once this runs on its own thread. cpptrace resolving a corrupt/unusual
    // stack is the realistic way this could throw; better to lose one trace than the server.
    std::thread([raw = std::move(raw)]()
    {
        try
        {
            LogResolvedStacktrace(raw.resolve());
        }
        catch (std::exception const& e)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "PrintStacktraceAsync: failed to resolve/log stacktrace: %s", e.what());
        }
        catch (...)
        {
            sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "PrintStacktraceAsync: failed to resolve/log stacktrace (unknown exception)");
        }
    }).detach();
#endif
}


#if COMPILER == COMPILER_MICROSOFT
#pragma warning(push)
#pragma warning(disable: 4702) // Disable unreachable code warning
#endif
[[noreturn]]
void MaNGOS::Errors::PrintStacktraceAndThrow(char const* filename, int line, char const* functionName, char const* failedExpression, char const* message)
{
    if (message)
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "%s:%i Error: Assertion in %s: %s (%s)", filename, line, functionName, failedExpression, message);
    else
        sLog.Out(LOG_BASIC, LOG_LVL_ERROR, "%s:%i Error: Assertion in %s: %s", filename, line, functionName, failedExpression);

    // Async (see PrintStacktraceAsync() comment in Errors.h): an ASSERT failure isn't
    // necessarily fatal in this codebase - WorldSession::Update() catches the
    // std::runtime_error thrown below per-packet and just kicks the offending player,
    // the server keeps running - so the DWARF symbol resolution must not block whichever
    // thread hit this assertion for multiple seconds. The throw right after is unaffected.
    MaNGOS::Errors::PrintStacktraceAsync(1, 32);

    std::string completeMessage = failedExpression;
    if (message)
        completeMessage += std::string(" Message: ") + message;

    throw std::runtime_error(completeMessage);

    // Just in case the std::runtime_error was ignored by a debugger, we throw an assert.
    assert("MANGOS_ASSERT throw was skipped" && false);
}
#if COMPILER == COMPILER_MICROSOFT
#pragma warning(pop)
#endif
