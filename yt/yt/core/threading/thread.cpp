#include "thread.h"

#include "private.h"

#include <yt/yt/core/actions/bind.h>

#include <yt/yt/core/misc/proc.h>

#include <library/cpp/yt/misc/tls.h>
#include <library/cpp/yt/system/exit.h>

#include <util/generic/size_literals.h>

#ifdef _linux_
    #include <sched.h>
#endif

#if defined(_unix_)
    #include <sys/mman.h>
#endif

#include <signal.h>

namespace NYT::NThreading {

////////////////////////////////////////////////////////////////////////////////

YT_DEFINE_THREAD_LOCAL(TThreadId, CurrentUniqueThreadId) ;
static std::atomic<TThreadId> UniqueThreadIdGenerator;

constinit const auto Logger = ThreadingLogger;

////////////////////////////////////////////////////////////////////////////////

TThread::TThread(
    TString threadName,
    TThreadOptions options)
    : ThreadName_(std::move(threadName))
    , Options_(std::move(options))
    , UniqueThreadId_(++UniqueThreadIdGenerator)
    , UnderlyingThread_(&StaticThreadMainTrampoline, this)
{ }

TThread::~TThread()
{
    Stop();
}

TThreadId TThread::GetThreadId() const
{
    return ThreadId_;
}

TString TThread::GetThreadName() const
{
    return ThreadName_;
}

bool TThread::StartSlow()
{
    auto guard = Guard(SpinLock_);

    if (Started_.load()) {
        return !Stopping_.load();
    }

    if (Stopping_.load()) {
        // Stopped without being started.
        return false;
    }

    ShutdownCookie_ = RegisterShutdownCallback(
        Format("Thread(%v)", ThreadName_),
        BIND_NO_PROPAGATE(&TThread::Stop, MakeWeak(this)),
        Options_.ShutdownPriority);
    if (!ShutdownCookie_) {
        Stopping_ = true;
        return false;
    }

    if (auto* logFile = TryGetShutdownLogFile()) {
        ::fprintf(logFile, "%s\t*** Starting thread (ThreadName: %s)\n",
            GetInstant().ToString().c_str(),
            ThreadName_.c_str());
    }

    StartPrologue();

    try {
        UnderlyingThread_.Start();
    } catch (const std::exception& ex) {
        fprintf(stderr, "%s\t*** Error starting thread (ThreadName: %s)\n*** %s\n",
            GetInstant().ToString().c_str(),
            ThreadName_.c_str(),
            ex.what());
        YT_ABORT();
    }

    Started_ = true;

    StartedEvent_.Wait();

    StartEpilogue();

    if (auto* logFile = TryGetShutdownLogFile()) {
        ::fprintf(logFile, "%s\t*** Thread started (ThreadName: %s, ThreadId: %" PRISZT ")\n",
            GetInstant().ToString().c_str(),
            ThreadName_.c_str(),
            ThreadId_);
    }

    return true;
}

bool TThread::CanWaitForThreadShutdown() const
{
    return
        CurrentUniqueThreadId() != UniqueThreadId_ &&
        GetShutdownThreadId() != ThreadId_;
}

void TThread::Stop()
{
    {
        auto guard = Guard(SpinLock_);
        auto alreadyStopping = Stopping_.exchange(true);
        if (!Started_) {
            return;
        }
        if (alreadyStopping) {
            guard.Release();
            // Avoid deadlock.
            if (CanWaitForThreadShutdown()) {
                if (auto* logFile = TryGetShutdownLogFile()) {
                    ::fprintf(logFile, "%s\t*** Waiting for an already stopping thread to finish (ThreadName: %s, ThreadId: %" PRISZT ", WaiterThreadId: %" PRISZT ")\n",
                        GetInstant().ToString().c_str(),
                        ThreadName_.c_str(),
                        ThreadId_,
                        GetCurrentThreadId());
                }
                StoppedEvent_.Wait();
            } else {
                if (auto* logFile = TryGetShutdownLogFile()) {
                    ::fprintf(logFile, "%s\t*** Cannot wait for an already stopping thread to finish (ThreadName: %s, ThreadId: %" PRISZT ", WaiterThreadId: %" PRISZT ")\n",
                        GetInstant().ToString().c_str(),
                        ThreadName_.c_str(),
                        ThreadId_,
                        GetCurrentThreadId());
                }
            }
            return;
        }
    }

    if (auto* logFile = TryGetShutdownLogFile()) {
        ::fprintf(logFile, "%s\t*** Stopping thread (ThreadName: %s, ThreadId: %" PRISZT ", RequesterThreadId: %" PRISZT ")\n",
            GetInstant().ToString().c_str(),
            ThreadName_.c_str(),
            ThreadId_,
            GetCurrentThreadId());
    }

    StopPrologue();

    // Avoid deadlock.
    if (CanWaitForThreadShutdown()) {
        if (auto* logFile = TryGetShutdownLogFile()) {
            ::fprintf(logFile, "%s\t*** Waiting for thread to stop (ThreadName: %s, ThreadId: %" PRISZT ", RequesterThreadId: %" PRISZT ")\n",
                GetInstant().ToString().c_str(),
                ThreadName_.c_str(),
                ThreadId_,
                GetCurrentThreadId());
        }
        UnderlyingThread_.Join();
    } else {
        if (auto* logFile = TryGetShutdownLogFile()) {
            ::fprintf(logFile, "%s\t*** Cannot wait for thread to stop; detaching (ThreadName: %s, ThreadId: %" PRISZT ", RequesterThreadId: %" PRISZT ")\n",
                GetInstant().ToString().c_str(),
                ThreadName_.c_str(),
                ThreadId_,
                GetCurrentThreadId());
        }
        UnderlyingThread_.Detach();
    }

    StopEpilogue();

    if (auto* logFile = TryGetShutdownLogFile()) {
        ::fprintf(logFile, "%s\t*** Thread stopped (ThreadName: %s, ThreadId: %" PRISZT ", RequesterThreadId: %" PRISZT ")\n",
            GetInstant().ToString().c_str(),
            ThreadName_.c_str(),
            ThreadId_,
            GetCurrentThreadId());
    }
}

void* TThread::StaticThreadMainTrampoline(void* opaque)
{
    reinterpret_cast<TThread*>(opaque)->ThreadMainTrampoline();
    return nullptr;
}

YT_PREVENT_TLS_CACHING void TThread::ThreadMainTrampoline()
{
    auto this_ = MakeStrong(this);

    ::TThread::SetCurrentThreadName(ThreadName_.c_str());

    ThreadId_ = GetCurrentThreadId();
    CurrentUniqueThreadId() = UniqueThreadId_;

    SetThreadPriority();
    ConfigureSignalHandlerStack();

    StartedEvent_.NotifyAll();

    class TExitInterceptor
    {
    public:
        ~TExitInterceptor()
        {
            if (Armed_ && std::uncaught_exceptions() == 0) {
                if (auto* logFile = TryGetShutdownLogFile()) {
                    ::fprintf(logFile, "%s\tThread exit interceptor triggered (ThreadId: %" PRISZT ")\n",
                        GetInstant().ToString().c_str(),
                        GetCurrentThreadId());
                }
                Shutdown();
            }
        }

        void Disarm()
        {
            Armed_ = false;
        }

    private:
        bool Armed_ = true;
    };

    thread_local TExitInterceptor Interceptor;

    if (Options_.ThreadInitializer) {
        Options_.ThreadInitializer();
    }

    ThreadMain();

    Interceptor.Disarm();

    StoppedEvent_.NotifyAll();
}

void TThread::StartPrologue()
{ }

void TThread::StartEpilogue()
{ }

void TThread::StopPrologue()
{ }

void TThread::StopEpilogue()
{ }

void TThread::SetThreadPriority()
{
    YT_VERIFY(ThreadId_ != InvalidThreadId);

#ifdef _linux_
    if (Options_.ThreadPriority == EThreadPriority::RealTime) {
        struct sched_param param{
            .sched_priority = 1
        };
        int result = sched_setscheduler(ThreadId_, SCHED_FIFO, &param);
        if (result == 0) {
            YT_LOG_DEBUG("Thread real-time priority enabled (ThreadName: %v)",
                ThreadName_);
        } else {
            YT_LOG_DEBUG(TError::FromSystem(), "Cannot enable thread real-time priority: sched_setscheduler failed (ThreadName: %v)",
                ThreadName_);
        }
    }
#else
    Y_UNUSED(Options_);
    Y_UNUSED(Logger);
#endif
}

#if defined(_unix_)
TThread::TSignalHandlerStack::TSignalHandlerStack(size_t size)
    : Size_(size)
{
    const size_t guardSize = GuardPageCount * GetPageSize();

    int flags =
#if defined(_darwin_)
        MAP_ANON | MAP_PRIVATE;
#else
        MAP_ANONYMOUS | MAP_PRIVATE;
#endif

    Base_ = reinterpret_cast<char*>(::mmap(
        0,
        guardSize * 2 + Size_,
        PROT_READ | PROT_WRITE,
        flags,
        -1,
        0));

    auto checkOom = [] {
        if (LastSystemError() == ENOMEM) {
            AbortProcessDramatically(
                EProcessExitCode::OutOfMemory,
                "Out-of-memory on signal handler stack allocation");
        }
    };

    if (Base_ == MAP_FAILED) {
        checkOom();
        YT_LOG_FATAL(TError::FromSystem(), "Failed to allocate signal handler stack (Size: %v)",
            Size_);
    }

    if (::mprotect(Base_, guardSize, PROT_NONE) == -1) {
        checkOom();
        YT_LOG_FATAL(TError::FromSystem(), "Failed to protect signal handler stack from below (GuardSize: %v, AddrToProtect: %v)",
            guardSize,
            Base_);
    }

    if (::mprotect(Base_ + guardSize + Size_, guardSize, PROT_NONE) == -1) {
        checkOom();
        YT_LOG_FATAL(TError::FromSystem(), "Failed to protect signal handler stack from above (GuardSize: %v, AddrToProtect: %v)",
            guardSize,
            Base_ + guardSize + Size_);
    }

#if !defined(_asan_enabled_) && !defined(_msan_enabled_) && defined(_unix_) && \
    (_XOPEN_SOURCE >= 500 || \
    /* Since glibc 2.12: */ _POSIX_C_SOURCE >= 200809L || \
    /* glibc <= 2.19: */ _BSD_SOURCE)
    auto* stackStart = Base_ + guardSize;
    stack_t stack{
        .ss_sp = stackStart,
        .ss_flags = 0,
        .ss_size = Size_,
    };

    YT_VERIFY(sigaltstack(&stack, nullptr) == 0);
#endif
}

TThread::TSignalHandlerStack::~TSignalHandlerStack()
{
    const size_t guardSize = GuardPageCount * GetPageSize();
    ::munmap(Base_, guardSize * 2 + Size_);
}
#endif

YT_PREVENT_TLS_CACHING void TThread::ConfigureSignalHandlerStack()
{
#if !defined(_asan_enabled_) && !defined(_msan_enabled_) && defined(_unix_) && \
    (_XOPEN_SOURCE >= 500 || \
    /* Since glibc 2.12: */ _POSIX_C_SOURCE >= 200809L || \
    /* glibc <= 2.19: */ _BSD_SOURCE)
    thread_local bool Configured;
    if (std::exchange(Configured, true)) {
        return;
    }

    // The size of of the custom stack to be provided for signal handlers.
    constexpr size_t SignalHandlerStackSize = 32_KB;

    SignalHandlerStack_ = std::make_unique<TSignalHandlerStack>(SignalHandlerStackSize);
#endif
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NThreading
