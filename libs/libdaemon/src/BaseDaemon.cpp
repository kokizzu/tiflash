// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/Config/ConfigProcessor.h>
#include <cxxabi.h>
#include <daemon/BaseDaemon.h>
#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <string.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>

#if USE_UNWIND
#ifndef USE_LLVM_LIBUNWIND
#define UNW_LOCAL_ONLY
#endif
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wextern-c-compat"
#endif
#include <libunwind.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#endif

#ifdef __APPLE__
// ucontext is not available without _XOPEN_SOURCE
#define _XOPEN_SOURCE
#endif
#include <Common/Exception.h>
#include <Common/TiFlashBuildInfo.h>
#include <Common/UnifiedLogFormatter.h>
#include <Common/getMultipleKeysFromConfig.h>
#include <Common/setThreadName.h>
#include <IO/Buffer/ReadBufferFromFileDescriptor.h>
#include <IO/Buffer/WriteBufferFromFileDescriptor.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteHelpers.h>
#include <Poco/AutoPtr.h>
#include <Poco/Condition.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/ErrorHandler.h>
#include <Poco/Exception.h>
#include <Poco/Ext/LevelFilterChannel.h>
#include <Poco/Ext/ReloadableSplitterChannel.h>
#include <Poco/Ext/SourceFilterChannel.h>
#include <Poco/Ext/ThreadNumber.h>
#include <Poco/Ext/TiFlashLogFileChannel.h>
#include <Poco/File.h>
#include <Poco/FormattingChannel.h>
#include <Poco/Logger.h>
#include <Poco/Message.h>
#include <Poco/NumberFormatter.h>
#include <Poco/Observer.h>
#include <Poco/Path.h>
#include <Poco/PatternFormatter.h>
#include <Poco/SplitterChannel.h>
#include <Poco/String.h>
#include <Poco/SyslogChannel.h>
#include <Poco/Util/AbstractConfiguration.h>
#include <Poco/Util/Application.h>
#include <Poco/Util/MapConfiguration.h>
#include <common/ErrorHandlers.h>
#include <common/logger_useful.h>
#include <common/logger_util.h>
#include <daemon/OwnPatternFormatter.h>
#include <fmt/format.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <ucontext.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <typeinfo>

#ifdef TIFLASH_LLVM_COVERAGE
extern "C" int __llvm_profile_write_file(void);
#endif

using Poco::AutoPtr;
using Poco::ConsoleChannel;
using Poco::FormattingChannel;
using Poco::Logger;
using Poco::Message;
using Poco::Util::AbstractConfiguration;

/** For transferring information from signal handler to a separate thread.
  * If you need to do something serious in case of a signal (example: write a message to the log),
  *  then sending information to a separate thread through pipe and doing all the stuff asynchronously
  *  - is probably the only safe method for doing it.
  * (Because it's only safe to use reentrant functions in signal handlers.)
  */
struct Pipe
{
    union
    {
        int fds[2]{};
        struct
        {
            int read_fd;
            int write_fd;
        };
    };

    Pipe()
    {
        read_fd = -1;
        write_fd = -1;

        if (0 != pipe(fds))
            DB::throwFromErrno("Cannot create pipe");
    }

    void close()
    {
        if (-1 != read_fd)
        {
            ::close(read_fd);
            read_fd = -1;
        }

        if (-1 != write_fd)
        {
            ::close(write_fd);
            write_fd = -1;
        }
    }

    ~Pipe() { close(); }
};


Pipe signal_pipe;


/** Reset signal handler to the default and send signal to itself.
  * It's called from user signal handler to write core dump.
  */
static void call_default_signal_handler(int sig)
{
    signal(sig, SIG_DFL);
    kill(getpid(), sig);
}


using ThreadNumber = decltype(Poco::ThreadNumber::get());
static const size_t buf_size = sizeof(int) + sizeof(siginfo_t) + sizeof(ucontext_t)
#if USE_UNWIND
    + sizeof(unw_context_t)
#endif
    + sizeof(ThreadNumber);

using signal_function = void(int, siginfo_t *, void *);

static void writeSignalIDtoSignalPipe(int sig)
{
    char buf[buf_size];
    DB::WriteBufferFromFileDescriptor out(signal_pipe.write_fd, buf_size, buf);
    DB::writeBinary(sig, out);
    out.next();
}

/** Signal handler for HUP / USR1 */
static void closeLogsSignalHandler(int sig, siginfo_t * /*info*/, void * /*context*/)
{
    writeSignalIDtoSignalPipe(sig);
}

static void terminateRequestedSignalHandler(int sig, siginfo_t * /*info*/, void * /*context*/)
{
    writeSignalIDtoSignalPipe(sig);
}


thread_local bool already_signal_handled = false;

/** Handler for "fault" signals. Send data about fault to separate thread to write into log.
  */
static void faultSignalHandler(int sig, siginfo_t * info, void * context)
{
    if (already_signal_handled)
        return;
    already_signal_handled = true;

    char buf[buf_size];
    DB::WriteBufferFromFileDescriptor out(signal_pipe.write_fd, buf_size, buf);

#if USE_UNWIND
    // different arch, different unwinder will have different definition for unwind
    // context; therefore, we catpure unw_context_t instead of ucontext_t
    unw_context_t unw_context;
    unw_getcontext(&unw_context);
#endif

    DB::writeBinary(sig, out);
    DB::writePODBinary(*info, out);
    DB::writePODBinary(*reinterpret_cast<const ucontext_t *>(context), out);
#if USE_UNWIND
    DB::writePODBinary(unw_context, out);
#endif
    DB::writeBinary(Poco::ThreadNumber::get(), out);

    out.next();

    /// The time that is usually enough for separate thread to print info into log.
    ::sleep(10);
#ifdef TIFLASH_LLVM_COVERAGE
    __llvm_profile_write_file();
#endif
    call_default_signal_handler(sig);
}


static bool already_printed_stack_trace = false;

#if USE_UNWIND
size_t backtraceLibUnwind(void ** out_frames, size_t max_frames, unw_context_t & unw_context)
{
    if (already_printed_stack_trace)
        return 0;

    unw_cursor_t cursor;

#ifdef USE_LLVM_LIBUNWIND
    // LLVM does not require UNW_INIT_SIGNAL_FRAME (assuming that signal frame CFIs are set correctly)
    // see https://lists.llvm.org/pipermail/llvm-dev/2021-December/154419.html
    if (unw_init_local(&cursor, &unw_context) < 0)
        return 0;
#else
    if (unw_init_local2(&cursor, &unw_context, UNW_INIT_SIGNAL_FRAME) < 0)
        return 0;
#endif

    size_t i = 0;
    for (; i < max_frames; ++i)
    {
        unw_word_t ip;
        unw_get_reg(&cursor, UNW_REG_IP, &ip);
        out_frames[i] = reinterpret_cast<void *>(ip);

        /// NOTE This triggers "AddressSanitizer: stack-buffer-overflow". Looks like false positive.
        /// It's Ok, because we use this method if the program is crashed nevertheless.
        if (!unw_step(&cursor))
            break;
    }

    return i;
}
#endif


/** The thread that read info about signal or std::terminate from pipe.
  * On HUP / USR1, close log files (for new files to be opened later).
  * On information about std::terminate, write it to log.
  * On other signals, write info to log.
  */
class SignalListener : public Poco::Runnable
{
public:
    enum Signals : int
    {
        StdTerminate = -1,
        StopThread = -2
    };

    explicit SignalListener(BaseDaemon & daemon_)
        : log(&Logger::get("BaseDaemon"))
        , daemon(daemon_)
    {}

    void run() override
    {
        setThreadName("SignalListener");

        char buf[buf_size];
        DB::ReadBufferFromFileDescriptor in(signal_pipe.read_fd, buf_size, buf);

        while (!in.eof())
        {
            int sig = 0;
            DB::readBinary(sig, in);

            if (sig == Signals::StopThread)
            {
                LOG_INFO(log, "Stop SignalListener thread");
                break;
            }
            else if (sig == SIGHUP || sig == SIGUSR1)
            {
                LOG_DEBUG(log, "Received signal to close logs.");
                BaseDaemon::instance().closeLogs();
                LOG_INFO(log, "Opened new log file after received signal.");
            }
            else if (sig == Signals::StdTerminate)
            {
                ThreadNumber thread_num;
                std::string message;

                DB::readBinary(thread_num, in);
                DB::readBinary(message, in);

                onTerminate(message, thread_num);
            }
            else if (sig == SIGINT || sig == SIGQUIT || sig == SIGTERM)
            {
                daemon.handleSignal(sig);
            }
            else
            {
                siginfo_t info;
                ucontext_t context;
#if USE_UNWIND
                unw_context_t unw_context;
#endif
                ThreadNumber thread_num;

                DB::readPODBinary(info, in);
                DB::readPODBinary(context, in);
#if USE_UNWIND
                DB::readPODBinary(unw_context, in);
#endif
                DB::readBinary(thread_num, in);
#if USE_UNWIND
                onFault(sig, info, context, unw_context, thread_num);
#else
                onFault(sig, info, context, thread_num);
#endif
            }
        }
    }

private:
    Logger * log;
    BaseDaemon & daemon;

private:
    void onTerminate(const std::string & message, ThreadNumber thread_num) const
    {
        LOG_ERROR(log, "(from thread {}) {}", thread_num, message);
    }
#if USE_UNWIND
    void onFault(int sig, siginfo_t & info, ucontext_t & context, unw_context_t & unw_context, ThreadNumber thread_num)
        const
#else
    void onFault(int sig, siginfo_t & info, ucontext_t & context, ThreadNumber thread_num) const
#endif
    {
        LOG_ERROR(log, "########################################");
        LOG_ERROR(log, "(from thread {}) Received signal {}({}).", thread_num, strsignal(sig), sig);

        void * caller_address = nullptr;

#if defined(__x86_64__)
/// Get the address at the time the signal was raised from the RIP (x86-64)
#if defined(__FreeBSD__)
        caller_address = reinterpret_cast<void *>(context.uc_mcontext.mc_rip);
#elif defined(__APPLE__)
        caller_address = reinterpret_cast<void *>(context.uc_mcontext->__ss.__rip);
#else
        caller_address = reinterpret_cast<void *>(context.uc_mcontext.gregs[REG_RIP]);
        auto err_mask = context.uc_mcontext.gregs[REG_ERR];
#endif
#elif defined(__aarch64__)
#if defined(__arm64__) || defined(__arm64) /// Apple arm cpu
        caller_address = reinterpret_cast<void *>(context.uc_mcontext->__ss.__pc);
#else /// arm server
        caller_address = reinterpret_cast<void *>(context.uc_mcontext.pc);
#endif
#endif

        switch (sig)
        {
        case SIGSEGV:
        {
            /// Print info about address and reason.
            if (nullptr == info.si_addr)
                LOG_ERROR(log, "Address: NULL pointer.");
            else
                LOG_ERROR(log, "Address: {}", info.si_addr);

#if defined(__x86_64__) && !defined(__FreeBSD__) && !defined(__APPLE__)
            if ((err_mask & 0x02))
                LOG_ERROR(log, "Access: write.");
            else
                LOG_ERROR(log, "Access: read.");
#endif

            switch (info.si_code)
            {
            case SEGV_ACCERR:
                LOG_ERROR(log, "Attempted access has violated the permissions assigned to the memory area.");
                break;
            case SEGV_MAPERR:
                LOG_ERROR(log, "Address not mapped to object.");
                break;
            default:
                LOG_ERROR(log, "Unknown si_code.");
                break;
            }
            break;
        }

        case SIGBUS:
        {
            switch (info.si_code)
            {
            case BUS_ADRALN:
                LOG_ERROR(log, "Invalid address alignment.");
                break;
            case BUS_ADRERR:
                LOG_ERROR(log, "Non-existant physical address.");
                break;
            case BUS_OBJERR:
                LOG_ERROR(log, "Object specific hardware error.");
                break;

                // Linux specific
#if defined(BUS_MCEERR_AR)
            case BUS_MCEERR_AR:
                LOG_ERROR(log, "Hardware memory error: action required.");
                break;
#endif
#if defined(BUS_MCEERR_AO)
            case BUS_MCEERR_AO:
                LOG_ERROR(log, "Hardware memory error: action optional.");
                break;
#endif

            default:
                LOG_ERROR(log, "Unknown si_code.");
                break;
            }
            break;
        }

        case SIGILL:
        {
            switch (info.si_code)
            {
            case ILL_ILLOPC:
                LOG_ERROR(log, "Illegal opcode.");
                break;
            case ILL_ILLOPN:
                LOG_ERROR(log, "Illegal operand.");
                break;
            case ILL_ILLADR:
                LOG_ERROR(log, "Illegal addressing mode.");
                break;
            case ILL_ILLTRP:
                LOG_ERROR(log, "Illegal trap.");
                break;
            case ILL_PRVOPC:
                LOG_ERROR(log, "Privileged opcode.");
                break;
            case ILL_PRVREG:
                LOG_ERROR(log, "Privileged register.");
                break;
            case ILL_COPROC:
                LOG_ERROR(log, "Coprocessor error.");
                break;
            case ILL_BADSTK:
                LOG_ERROR(log, "Internal stack error.");
                break;
            default:
                LOG_ERROR(log, "Unknown si_code.");
                break;
            }
            break;
        }

        case SIGFPE:
        {
            switch (info.si_code)
            {
            case FPE_INTDIV:
                LOG_ERROR(log, "Integer divide by zero.");
                break;
            case FPE_INTOVF:
                LOG_ERROR(log, "Integer overflow.");
                break;
            case FPE_FLTDIV:
                LOG_ERROR(log, "Floating point divide by zero.");
                break;
            case FPE_FLTOVF:
                LOG_ERROR(log, "Floating point overflow.");
                break;
            case FPE_FLTUND:
                LOG_ERROR(log, "Floating point underflow.");
                break;
            case FPE_FLTRES:
                LOG_ERROR(log, "Floating point inexact result.");
                break;
            case FPE_FLTINV:
                LOG_ERROR(log, "Floating point invalid operation.");
                break;
            case FPE_FLTSUB:
                LOG_ERROR(log, "Subscript out of range.");
                break;
            default:
                LOG_ERROR(log, "Unknown si_code.");
                break;
            }
            break;
        }
        }

        if (already_printed_stack_trace)
            return;

        static constexpr size_t max_frames = 50;
        size_t frames_size = 0;
        void * frames[max_frames];

#if USE_UNWIND
        frames_size = backtraceLibUnwind(frames, max_frames, unw_context);
        UNUSED(caller_address);
#else
        /// No libunwind means no backtrace, because we are in a different thread from the one where the signal happened.
        /// So at least print the function where the signal happened.
        if (caller_address)
        {
            frames[0] = caller_address;
            frames_size = 1;
        }
#endif

        DB::FmtBuffer output;

        for (size_t f = 0; f < frames_size; ++f)
        {
            output.append("\n");
            auto demangle_func = [](const char * name) {
                int status = 0;
                // __cxa_demangle will leak memory; but we are failing anyway
                // freeing memory may increase possibilities to trigger other errors
                auto * result = abi::__cxa_demangle(name, nullptr, nullptr, &status);
                return std::pair<const char *, int>{result, status};
            };
            StackTrace::addr2line(demangle_func, output, frames[f]);
        }
        LOG_ERROR(log, output.toString());
    }
};


/** To use with std::set_terminate.
  * Collects slightly more info than __gnu_cxx::__verbose_terminate_handler,
  *  and send it to pipe. Other thread will read this info from pipe and asynchronously write it to log.
  * Look at libstdc++-v3/libsupc++/vterminate.cc for example.
  */
static void terminate_handler()
{
    static thread_local bool terminating = false;
    if (terminating)
    {
        abort();
        return; /// Just for convenience.
    }

    terminating = true;

    std::stringstream log;

    std::type_info * t = abi::__cxa_current_exception_type();
    if (t)
    {
        /// Note that "name" is the mangled name.
        char const * name = t->name();
        {
            int status = -1;
            char * dem = nullptr;

            dem = abi::__cxa_demangle(name, nullptr, nullptr, &status);

            log << "Terminate called after throwing an instance of " << (status == 0 ? dem : name) << std::endl;

            if (status == 0)
                free(dem); // NOLINT(cppcoreguidelines-no-malloc)
        }

        already_printed_stack_trace = true;

        /// If the exception is derived from std::exception, we can give more information.
        try
        {
            throw;
        }
        catch (DB::Exception & e)
        {
            log << "Code: " << e.code() << ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what()
                << std::endl;
        }
        catch (Poco::Exception & e)
        {
            log << "Code: " << e.code() << ", e.displayText() = " << e.displayText() << ", e.what() = " << e.what()
                << std::endl;
        }
        catch (const std::exception & e)
        {
            log << "what(): " << e.what() << std::endl;
        }
        catch (...)
        {}

        log << "Stack trace:\n\n" << StackTrace().toString() << std::endl;
    }
    else
    {
        log << "Terminate called without an active exception" << std::endl;
    }

    static const size_t buf_size = 1024;

    std::string log_message = log.str();
    if (log_message.size() > buf_size - 16)
        log_message.resize(buf_size - 16);

    char buf[buf_size];
    DB::WriteBufferFromFileDescriptor out(signal_pipe.write_fd, buf_size, buf);

    DB::writeBinary(static_cast<int>(SignalListener::StdTerminate), out);
    DB::writeBinary(Poco::ThreadNumber::get(), out);
    DB::writeBinary(log_message, out);
    out.next();

    abort();
}


static std::string createDirectory(const std::string & file)
{
    auto path = Poco::Path(file).makeParent();
    if (path.toString().empty())
        return "";
    Poco::File(path).createDirectories();
    return path.toString();
}

static bool tryCreateDirectories(Poco::Logger * logger, const std::string & path)
{
    try
    {
        Poco::File(path).createDirectories();
        return true;
    }
    catch (...)
    {
        LOG_WARNING(logger, "when creating {}, {}", path, DB::getCurrentExceptionMessage(true));
    }
    return false;
}

void BaseDaemon::reloadConfiguration()
{
    // when config-file is not specified and config.toml does not exist, we do not load config.
    if (!config().has("config-file"))
    {
        Poco::File f("config.toml");
        if (!f.exists())
            return;
    }

    /** If the program is not run in daemon mode and 'config-file' is not specified,
      *  then we use config from 'config.toml' file in current directory,
      *  but will log to console (or use parameters --log-file, --errorlog-file from command line)
      *  instead of using files specified in config.xml.
      * (It's convenient to log in console when you start server without any command line parameters.)
      */
    config_path = config().getString("config-file", "config.toml");
    loaded_config = ConfigProcessor(config_path, true).loadConfig();
    if (last_configuration != nullptr)
        config().removeConfiguration(last_configuration);
    last_configuration = loaded_config.configuration.duplicate();
    config().add(last_configuration, PRIO_DEFAULT, false);
}


/// For creating and destroying unique_ptr of incomplete type.
BaseDaemon::BaseDaemon() = default;


BaseDaemon::~BaseDaemon()
{
    writeSignalIDtoSignalPipe(SignalListener::StopThread);
    signal_listener_thread.join();
    signal_pipe.close();
}


void BaseDaemon::terminate()
{
    if (::kill(Poco::Process::id(), SIGTERM) != 0)
    {
        throw Poco::SystemException("cannot terminate process");
    }
}

void BaseDaemon::kill()
{
    pid.clear();
    Poco::Process::kill(getpid());
}

void BaseDaemon::sleep(double seconds)
{
    wakeup_event.reset();
    wakeup_event.tryWait(seconds * 1000);
}

void BaseDaemon::wakeup()
{
    wakeup_event.set();
}

void BaseDaemon::buildLoggers(Poco::Util::AbstractConfiguration & config)
{
    auto current_logger = config.getString("logger", "(null)");
    if (config_logger == current_logger)
        return;
    config_logger = current_logger;

    bool is_daemon = config.getBool("application.runAsDaemon", false);

    // Split log, error log and tracing log.
    Poco::AutoPtr<Poco::ReloadableSplitterChannel> split = new Poco::ReloadableSplitterChannel;

    auto log_level = Utils::normalizeLogLevel(config.getString("logger.level", "info"));
    const auto log_path = config.getString("logger.log", "");
    if (!log_path.empty())
    {
        createDirectory(log_path);
        std::cerr << "Logging " << log_level << " to " << log_path << std::endl;

        // Set up two channel chains.
        Poco::AutoPtr<Poco::Formatter> pf = new DB::UnifiedLogFormatter<false>();
        Poco::AutoPtr<FormattingChannel> log = new FormattingChannel(pf);
        log_file = new Poco::TiFlashLogFileChannel;
        log_file->setProperty(Poco::FileChannel::PROP_PATH, Poco::Path(log_path).absolute().toString());
        log_file->setProperty(Poco::FileChannel::PROP_ROTATION, config.getRawString("logger.size", "100M"));
        log_file->setProperty(Poco::FileChannel::PROP_TIMES, "local");
        log_file->setProperty(Poco::FileChannel::PROP_ARCHIVE, "timestamp");
        log_file->setProperty(
            Poco::FileChannel::PROP_COMPRESS,
            /*config.getRawString("logger.compress", "true")*/ "true");
        log_file->setProperty(Poco::FileChannel::PROP_PURGECOUNT, config.getRawString("logger.count", "10"));
        log_file->setProperty(Poco::FileChannel::PROP_FLUSH, config.getRawString("logger.flush", "true"));
        log_file->setProperty(
            Poco::FileChannel::PROP_ROTATEONOPEN,
            config.getRawString("logger.rotateOnOpen", "false"));
        log->setChannel(log_file);
        split->addChannel(log);
        log_file->open();
    }

    const auto errorlog_path = config.getString("logger.errorlog", "");
    if (!errorlog_path.empty())
    {
        createDirectory(errorlog_path);
        std::cerr << "Logging errors to " << errorlog_path << std::endl;
        Poco::AutoPtr<Poco::LevelFilterChannel> level = new Poco::LevelFilterChannel;
        level->setLevel(Message::PRIO_NOTICE);
        Poco::AutoPtr<Poco::Formatter> pf = new DB::UnifiedLogFormatter<false>();
        Poco::AutoPtr<FormattingChannel> errorlog = new FormattingChannel(pf);
        error_log_file = new Poco::TiFlashLogFileChannel;
        error_log_file->setProperty(Poco::FileChannel::PROP_PATH, Poco::Path(errorlog_path).absolute().toString());
        error_log_file->setProperty(Poco::FileChannel::PROP_ROTATION, config.getRawString("logger.size", "100M"));
        error_log_file->setProperty(Poco::FileChannel::PROP_TIMES, "local");
        error_log_file->setProperty(Poco::FileChannel::PROP_ARCHIVE, "timestamp");
        error_log_file->setProperty(
            Poco::FileChannel::PROP_COMPRESS,
            /*config.getRawString("logger.compress", "true")*/ "true");
        error_log_file->setProperty(Poco::FileChannel::PROP_PURGECOUNT, config.getRawString("logger.count", "10"));
        error_log_file->setProperty(Poco::FileChannel::PROP_FLUSH, config.getRawString("logger.flush", "true"));
        error_log_file->setProperty(
            Poco::FileChannel::PROP_ROTATEONOPEN,
            config.getRawString("logger.rotateOnOpen", "false"));
        errorlog->setChannel(error_log_file);
        level->setChannel(errorlog);
        split->addChannel(level);
        errorlog->open();
    }

    const auto tracing_log_path = config.getString("logger.tracing_log", "");
    if (!tracing_log_path.empty())
    {
        createDirectory(tracing_log_path);
        std::cerr << "Logging tracing log to " << tracing_log_path << std::endl;
        /// to filter the tracing log.
        Poco::AutoPtr<Poco::SourceFilterChannel> source = new Poco::SourceFilterChannel;
        source->setSource(DB::tracing_log_source);
        Poco::AutoPtr<Poco::Formatter> pf = new DB::UnifiedLogFormatter<false>();
        Poco::AutoPtr<FormattingChannel> tracing_log = new FormattingChannel(pf);
        tracing_log_file = new Poco::TiFlashLogFileChannel;
        tracing_log_file->setProperty(Poco::FileChannel::PROP_PATH, Poco::Path(tracing_log_path).absolute().toString());
        tracing_log_file->setProperty(Poco::FileChannel::PROP_ROTATION, config.getRawString("logger.size", "100M"));
        tracing_log_file->setProperty(Poco::FileChannel::PROP_TIMES, "local");
        tracing_log_file->setProperty(Poco::FileChannel::PROP_ARCHIVE, "timestamp");
        tracing_log_file->setProperty(
            Poco::FileChannel::PROP_COMPRESS,
            /*config.getRawString("logger.compress", "true")*/ "true");
        tracing_log_file->setProperty(Poco::FileChannel::PROP_PURGECOUNT, config.getRawString("logger.count", "10"));
        tracing_log_file->setProperty(Poco::FileChannel::PROP_FLUSH, config.getRawString("logger.flush", "true"));
        tracing_log_file->setProperty(
            Poco::FileChannel::PROP_ROTATEONOPEN,
            config.getRawString("logger.rotateOnOpen", "false"));
        tracing_log->setChannel(tracing_log_file);
        source->setChannel(tracing_log);
        split->addChannel(source);
        tracing_log->open();
    }

    /// "dynamic_layer_selection" is needed only for Yandex.Metrika, that share part of ClickHouse code.
    /// We don't need this configuration parameter.

    if (config.getBool("logger.use_syslog", false) || config.getBool("dynamic_layer_selection", false))
    {
        Poco::AutoPtr<OwnPatternFormatter> pf = new OwnPatternFormatter(this, OwnPatternFormatter::ADD_LAYER_TAG);
        pf->setProperty("times", "local");
        Poco::AutoPtr<FormattingChannel> log = new FormattingChannel(pf);
        syslog_channel = new Poco::SyslogChannel(
            commandName(),
            Poco::SyslogChannel::SYSLOG_CONS | Poco::SyslogChannel::SYSLOG_PID,
            Poco::SyslogChannel::SYSLOG_DAEMON);
        log->setChannel(syslog_channel);
        split->addChannel(log);
        syslog_channel->open();
    }

    bool should_log_to_console = isatty(STDIN_FILENO) || isatty(STDERR_FILENO);
    bool enable_colors = isatty(STDERR_FILENO);

    if (config.getBool("logger.console", false)
        || (!config.hasProperty("logger.console") && !is_daemon && should_log_to_console))
    {
        Poco::AutoPtr<ConsoleChannel> file = new ConsoleChannel;
        Poco::AutoPtr<Poco::Formatter> pf;
        if (enable_colors)
            pf = new DB::UnifiedLogFormatter<true>();
        else
            pf = new DB::UnifiedLogFormatter<false>();
        Poco::AutoPtr<FormattingChannel> log = new FormattingChannel(pf);
        log->setChannel(file);
        split->addChannel(log);
    }

    split->open();
    logger().close();
    logger().setChannel(split);

    // Global logging level (it can be overridden for specific loggers).
    logger().setLevel(log_level);

    // Set level to all already created loggers
    std::vector<std::string> names;
    Logger::root().names(names);
    for (const auto & name : names)
    {
        Logger & cur_logger = Logger::root().get(name);
        cur_logger.setLevel(log_level);
        Poco::Channel * cur_logger_channel = cur_logger.getChannel();
        if (!cur_logger_channel)
        {
            continue;
        }
        // only loggers created after buildLoggers() need to change properties, types of channel in them must be ReloadableSplitterChannel
        if (typeid(*cur_logger_channel) == typeid(Poco::ReloadableSplitterChannel))
        {
            auto * splitter_channel = dynamic_cast<Poco::ReloadableSplitterChannel *>(cur_logger_channel);
            splitter_channel->changeProperties(config);
        }
    }

    // Attach to the root logger.
    Logger::root().setLevel(log_level);
    Logger::root().setChannel(logger().getChannel());

    // Explicitly specified log levels for specific loggers.
    AbstractConfiguration::Keys levels;
    config.keys("logger.levels", levels);

    if (!levels.empty())
        for (auto & level : levels)
            Logger::get(level).setLevel(config.getString("logger.levels." + level, "trace"));
}


void BaseDaemon::closeLogs()
{
    if (log_file)
        log_file->close();
    if (error_log_file)
        error_log_file->close();
    if (tracing_log_file)
        tracing_log_file->close();

    if (!log_file)
        logger().warning("Logging to console but received signal to close log file (ignoring).");
}

std::string BaseDaemon::getDefaultCorePath() const
{
    return "/opt/cores/";
}

void BaseDaemon::initialize(Application & self)
{
    ServerApplication::initialize(self);

    {
        /// Parsing all args and converting to config layer
        /// Test: -- --1=1 --1=2 --3 5 7 8 -9 10 -11=12 14= 15== --16==17 --=18 --19= --20 21 22 --23 --24 25 --26 -27 28 ---29=30 -- ----31 32 --33 3-4
        Poco::AutoPtr<Poco::Util::MapConfiguration> map_config = new Poco::Util::MapConfiguration;
        std::string key;
        for (const auto & arg : argv())
        {
            auto key_start = arg.find_first_not_of('-');
            auto pos_minus = arg.find('-');
            auto pos_eq = arg.find('=');

            // old saved '--key', will set to some true value "1"
            if (!key.empty() && pos_minus != std::string::npos && pos_minus < key_start)
            {
                map_config->setString(key, "1");
                key = "";
            }

            if (pos_eq == std::string::npos)
            {
                if (!key.empty())
                {
                    if (pos_minus == std::string::npos || pos_minus > key_start)
                    {
                        map_config->setString(key, arg);
                    }
                    key = "";
                }
                if (pos_minus != std::string::npos && key_start != std::string::npos && pos_minus < key_start)
                    key = arg.substr(key_start);
                continue;
            }
            else
            {
                key = "";
            }

            if (key_start == std::string::npos)
                continue;

            if (pos_minus > key_start)
                continue;

            key = arg.substr(key_start, pos_eq - key_start);
            if (key.empty())
                continue;
            std::string value;
            if (arg.size() > pos_eq)
                value = arg.substr(pos_eq + 1);

            map_config->setString(key, value);
            key = "";
        }
        /// now highest priority (lowest value) is PRIO_APPLICATION = -100, we want higher!
        config().add(map_config, PRIO_APPLICATION - 100);
    }

    bool is_daemon = config().getBool("application.runAsDaemon", false);

    if (is_daemon)
    {
        /** When creating pid file and looking for config, will search for paths relative to the working path of the program when started.
          */
        std::string path = Poco::Path(config().getString("application.path")).setFileName("").toString();
        if (0 != chdir(path.c_str()))
            throw Poco::Exception("Cannot change directory to " + path);
    }

    reloadConfiguration();

    /// This must be done before creation of any files (including logs).
    if (config().has("umask"))
    {
        std::string umask_str = config().getString("umask");
        mode_t umask_num = 0;
        std::stringstream stream;
        stream << umask_str;
        stream >> std::oct >> umask_num;

        umask(umask_num);
    }

    /// Write core dump on crash.
    {
        struct rlimit rlim
        {
        };
        if (getrlimit(RLIMIT_CORE, &rlim))
            throw Poco::Exception("Cannot getrlimit");
        /// 1 GiB by default. If more - it writes to disk too long.
        rlim.rlim_cur = config().getUInt64("core_dump.size_limit", 1024 * 1024 * 1024);

        if (setrlimit(RLIMIT_CORE, &rlim))
        {
            std::string message = "Cannot set max size of core file to " + std::to_string(rlim.rlim_cur);
#if !defined(ADDRESS_SANITIZER) && !defined(THREAD_SANITIZER) && !defined(MEMORY_SANITIZER) && !defined(SANITIZER)
            throw Poco::Exception(message);
#else
            /// It doesn't work under address/thread sanitizer. http://lists.llvm.org/pipermail/llvm-bugs/2013-April/027880.html
            std::cerr << message << std::endl;
#endif
        }
    }

    /// This must be done before any usage of DateLUT. In particular, before any logging.
    if (config().has("timezone"))
    {
        if (0 != setenv("TZ", config().getString("timezone").data(), 1))
            throw Poco::Exception("Cannot setenv TZ variable");

        tzset();
    }

    std::string log_path = config().getString("logger.log", "");
    if (!log_path.empty())
        log_path = Poco::Path(log_path).setFileName("").toString();

    if (is_daemon)
    {
        /** Redirect stdout, stderr to separate files in the log directory.
          * Some libraries write to stderr in case of errors in debug mode,
          *  and this output makes sense even if the program is run in daemon mode.
          * We have to do it before buildLoggers, for errors on logger initialization will be written to these files.
          */
        if (!log_path.empty())
        {
            std::string stdout_path = log_path + "/stdout";
            if (!freopen(stdout_path.c_str(), "a+", stdout))
                throw Poco::OpenFileException("Cannot attach stdout to " + stdout_path);

            std::string stderr_path = log_path + "/stderr";
            if (!freopen(stderr_path.c_str(), "a+", stderr))
                throw Poco::OpenFileException("Cannot attach stderr to " + stderr_path);
        }

        /// Create pid file.
        if (is_daemon && config().has("pid"))
            pid.seed(config().getString("pid"));
    }

    /// Change path for logging.
    if (!log_path.empty())
    {
        std::string path = createDirectory(log_path);
        if (is_daemon && chdir(path.c_str()) != 0)
            throw Poco::Exception("Cannot change directory to " + path);
    }
    else
    {
        if (is_daemon && chdir("/tmp") != 0)
            throw Poco::Exception("Cannot change directory to /tmp");
    }

    buildLoggers(config());

    if (is_daemon)
    {
        /** Change working directory to the directory to write core dumps.
          * We have to do it after buildLoggers, because there is the case when config files was in current directory.
          */

        std::string core_path = config().getString("core_path", "");
        if (core_path.empty())
            core_path = getDefaultCorePath();

        tryCreateDirectories(&logger(), core_path);

        Poco::File cores = core_path;
        if (!(cores.exists() && cores.isDirectory()))
        {
            core_path = !log_path.empty() ? log_path : "/opt/";
            tryCreateDirectories(&logger(), core_path);
        }

        if (0 != chdir(core_path.c_str()))
            throw Poco::Exception("Cannot change directory to " + core_path);
    }

    std::set_terminate(terminate_handler);

    /// We want to avoid SIGPIPE when working with sockets and pipes, and just handle return value/errno instead.
    {
        sigset_t sig_set;
        if (sigemptyset(&sig_set) || sigaddset(&sig_set, SIGPIPE) || pthread_sigmask(SIG_BLOCK, &sig_set, nullptr))
            throw Poco::Exception("Cannot block signal.");
    }

    /// Setup signal handlers.
    auto add_signal_handler = [](const std::vector<int> & signals, signal_function handler) {
        struct sigaction sa
        {
        };
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = handler;
        sa.sa_flags = SA_SIGINFO;

        {
            if (sigemptyset(&sa.sa_mask))
                throw Poco::Exception("Cannot set signal handler.");

            for (auto signal : signals)
                if (sigaddset(&sa.sa_mask, signal))
                    throw Poco::Exception("Cannot set signal handler.");

            for (auto signal : signals)
                if (sigaction(signal, &sa, nullptr))
                    throw Poco::Exception("Cannot set signal handler.");
        }
    };

    add_signal_handler({SIGABRT, SIGSEGV, SIGILL, SIGBUS, SIGSYS, SIGFPE, SIGPIPE}, faultSignalHandler);
    add_signal_handler({SIGHUP, SIGUSR1}, closeLogsSignalHandler);
    add_signal_handler({SIGINT, SIGQUIT, SIGTERM}, terminateRequestedSignalHandler);

    /// Set up Poco ErrorHandler for Poco Threads.
    static KillingErrorHandler killing_error_handler;
    Poco::ErrorHandler::set(&killing_error_handler);

    logVersion();

    signal_listener = std::make_unique<SignalListener>(*this);
    signal_listener_thread.start(*signal_listener);
}

void BaseDaemon::logVersion() const
{
    auto * log = &Logger::root();
    LOG_INFO(log, "Welcome to TiFlash");
    std::stringstream ss;
    TiFlashBuildInfo::outputDetail(ss);
    LOG_INFO(log, "TiFlash build info: {}", ss.str());
}

/// Used for exitOnTaskError()
void BaseDaemon::handleNotification(Poco::TaskFailedNotification * _tfn)
{
    task_failed = true;
    AutoPtr<Poco::TaskFailedNotification> fn(_tfn);
    Logger * lg = &(logger());
    LOG_ERROR(
        lg,
        "Task '{}' failed. Daemon is shutting down. Reason - {}",
        fn->task()->name(),
        fn->reason().displayText());
    ServerApplication::terminate();
}

void BaseDaemon::defineOptions(Poco::Util::OptionSet & _options)
{
    Poco::Util::ServerApplication::defineOptions(_options);

    _options.addOption(Poco::Util::Option("config-file", "C", "load configuration from a given file")
                           .required(false)
                           .repeatable(false)
                           .argument("<file>")
                           .binding("config-file"));

    _options.addOption(Poco::Util::Option("log-file", "L", "use given log file")
                           .required(false)
                           .repeatable(false)
                           .argument("<file>")
                           .binding("logger.log"));

    _options.addOption(Poco::Util::Option("errorlog-file", "E", "use given log file for errors only")
                           .required(false)
                           .repeatable(false)
                           .argument("<file>")
                           .binding("logger.errorlog"));

    _options.addOption(Poco::Util::Option("tracing-log-file", "T", "use given log file for mpp task tracing only")
                           .required(false)
                           .repeatable(false)
                           .argument("<file>")
                           .binding("logger.tracing_log"));

    _options.addOption(Poco::Util::Option("pid-file", "P", "use given pidfile")
                           .required(false)
                           .repeatable(false)
                           .argument("<file>")
                           .binding("pid"));
}

bool isPidRunning(pid_t pid)
{
    return getpgid(pid) >= 0;
}

void BaseDaemon::PID::seed(const std::string & file_)
{
    file = Poco::Path(file_).absolute().toString();
    Poco::File poco_file(file);

    if (poco_file.exists())
    {
        pid_t pid_read = 0;
        {
            std::ifstream in(file);
            if (in.good())
            {
                in >> pid_read;
                if (pid_read && isPidRunning(pid_read))
                    throw Poco::Exception(
                        "Pid file exists and program running with pid = " + std::to_string(pid_read)
                        + ", should not start daemon.");
            }
        }
        std::cerr << "Old pid file exists (with pid = " << pid_read << "), removing." << std::endl;
        poco_file.remove();
    }

    int fd = open(file.c_str(), O_CREAT | O_EXCL | O_WRONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    if (-1 == fd)
    {
        file.clear();
        if (EEXIST == errno)
            throw Poco::Exception("Pid file exists, should not start daemon.");
        throw Poco::CreateFileException("Cannot create pid file.");
    }

    try
    {
        std::stringstream s;
        s << getpid();
        if (static_cast<ssize_t>(s.str().size()) != write(fd, s.str().c_str(), s.str().size()))
            throw Poco::Exception("Cannot write to pid file.");
    }
    catch (...)
    {
        close(fd);
        throw;
    }

    close(fd);
}

void BaseDaemon::PID::clear()
{
    if (!file.empty())
    {
        Poco::File(file).remove();
        file.clear();
    }
}

void BaseDaemon::handleSignal(int signal_id)
{
    if (signal_id == SIGINT || signal_id == SIGQUIT || signal_id == SIGTERM)
    {
        std::unique_lock<std::mutex> lock(signal_handler_mutex);
        {
            ++terminate_signals_counter;
            sigint_signals_counter += signal_id == SIGINT;
            signal_event.notify_all();
        }

        onInterruptSignals(signal_id);
    }
    else
        throw DB::Exception(std::string("Unsupported signal: ") + strsignal(signal_id));
}

void BaseDaemon::onInterruptSignals(int signal_id)
{
    is_cancelled = true;
    LOG_INFO(&logger(), "Received termination signal ({})", strsignal(signal_id));

    if (sigint_signals_counter >= 2)
    {
        LOG_INFO(&logger(), "Received second signal Interrupt. Immediately terminate.");
        kill();
    }
}


void BaseDaemon::waitForTerminationRequest()
{
    std::unique_lock<std::mutex> lock(signal_handler_mutex);
    signal_event.wait(lock, [this]() { return terminate_signals_counter > 0; });
}
