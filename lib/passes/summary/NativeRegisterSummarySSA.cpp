#include "notdec-bin2llvm/passes/summary/NativeRegisterSummarySSA.h"

#include "notdec-bin2llvm/NativeAbi.h"
#include "notdec-bin2llvm/passes/summary/NativeRegisterSummary.h"
#include "notdec-bin2llvm/passes/summary/NativeStackFrame.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace notdec::bin2llvm {
namespace {

struct RegisterUnit {
  llvm::GlobalVariable *Global = nullptr;
  std::string Space;
  std::string Name;
  uint64_t Offset = 0;
  uint64_t Size = 0;
};

struct RegisterAccess {
  const RegisterUnit *Unit = nullptr;
  bool IsRegisterAccess = false;
  bool IsStorageValue = false;
};

struct SummaryRegisterFact {
  bool ReadEntry = false;
  bool MayEntry = true;
  bool MayNonEntry = false;
  bool ExitDemand = false;
};

struct FunctionSummaryFacts {
  std::map<std::string, SummaryRegisterFact> Registers;
};

struct AbiFacts {
  std::set<std::string> Inputs;
  std::vector<std::string> InputsInOrder;
  std::set<std::string> Outputs;
  std::vector<std::string> OutputsInOrder;
  std::set<std::string> InternalParamRegisters;
  std::set<std::string> InternalReturnRegisters;
  std::set<std::string> Unaffected;
  std::set<std::string> KilledByCall;
};

enum class CallRegisterEffect {
  Preserve,
  ReturnValue,
  Clobber,
  Unknown,
};

using BlockRegKey = std::pair<llvm::BasicBlock *, llvm::GlobalVariable *>;
using CallValueKey =
    std::tuple<llvm::Instruction *, llvm::GlobalVariable *, std::string>;

struct CallArgStoreBinding {
  llvm::StoreInst *Store = nullptr;
  const RegisterUnit *Unit = nullptr;
  llvm::Value *Value = nullptr;
  unsigned Index = 0;
};

struct KnownExternalPrototype {
  unsigned FixedArgs = 0;
  bool VarArg = false;
  bool NoReturn = false;
  unsigned MaxReturnRegisters = 1;
};

struct SignatureShape {
  std::vector<const RegisterUnit *> Params;
  std::vector<const RegisterUnit *> Returns;
  bool VarArg = false;
};

struct SignatureRewriteState {
  std::map<llvm::Function *, SignatureShape> Shapes;
  std::map<llvm::CallBase *, std::vector<CallArgStoreBinding>> CallArgs;
  std::map<llvm::CallBase *, std::map<std::string, llvm::CallInst *>>
      ReturnHelpers;
  std::map<llvm::Function *,
           std::map<llvm::ReturnInst *, std::vector<llvm::Value *>>>
      FunctionReturns;
  std::map<llvm::Value *, llvm::Value *> ValueMap;
  std::set<llvm::StoreInst *> StoresToErase;
  // Calls rebuilt by SummarySSA already carry their ABI inputs as LLVM
  // operands, so the later register-store liveness pass must not treat them as
  // users of @RDI/@RSI/... globals.
  std::set<const llvm::CallBase *> RewrittenCalls;
};

llvm::Value *frozenPoisonBefore(llvm::Instruction &insertBefore,
                                llvm::Type *type, llvm::Twine name) {
  llvm::IRBuilder<> builder(&insertBefore);
  return builder.CreateFreeze(llvm::PoisonValue::get(type), name);
}

llvm::Value *frozenPoisonAt(llvm::IRBuilder<> &builder, llvm::Type *type,
                            llvm::Twine name) {
  return builder.CreateFreeze(llvm::PoisonValue::get(type), name);
}

const std::map<llvm::StringRef, KnownExternalPrototype> &
knownExternalPrototypes() {
  static const std::map<llvm::StringRef, KnownExternalPrototype> prototypes = {
      {"__assert_fail", {4, false, true}},
      {"__ctype_b_loc", {0, false, false}},
      {"__ctype_tolower_loc", {0, false, false}},
      {"__ctype_toupper_loc", {0, false, false}},
      {"__cxa_atexit", {3, false, false}},
      {"__cxa_finalize", {1, false, false}},
      {"__errno_location", {0, false, false}},
      {"__explicit_bzero_chk", {3, false, false}},
      {"__fdelt_chk", {1, false, false}},
      {"__fgets_chk", {4, false, false}},
      {"__fprintf_chk", {3, true, false}},
      {"__getdelim", {4, false, false}},
      {"__isoc23_sscanf", {2, true, false}},
      {"__isoc23_strtol", {3, false, false}},
      {"__isoc23_strtoll", {3, false, false}},
      {"__isoc23_strtoul", {3, false, false}},
      {"__isoc23_strtoull", {3, false, false}},
      {"__isoc99_sscanf", {2, true, false}},
      {"__longjmp_chk", {2, false, true}},
      {"__memcpy_chk", {4, false, false}},
      {"__memmove_chk", {4, false, false}},
      {"__memset_chk", {4, false, false}},
      {"__open64_2", {2, false, false}},
      {"__printf_chk", {2, true, false}},
      {"__poll_chk", {4, false, false}},
      {"__read_chk", {4, false, false}},
      {"__register_atfork", {4, false, false}},
      {"__sched_cpucount", {2, false, false}},
      {"__snprintf_chk", {4, true, false}},
      {"__sprintf_chk", {3, true, false}},
      {"__strcat_chk", {3, false, false}},
      {"__strcpy_chk", {3, false, false}},
      {"__strncpy_chk", {4, false, false}},
      {"__sigsetjmp", {2, false, false}},
      {"__syslog_chk", {2, true, false}},
      {"__sysconf", {1, false, false}},
      {"__stack_chk_fail", {0, false, true}},
      {"__tls_get_addr", {1, false, false}},
      {"__vasprintf_chk", {3, true, false}},
      {"__xpg_strerror_r", {3, false, false}},
      {"_exit", {1, false, true}},
      {"abort", {0, false, true}},
      {"ERR_error_string_n", {3, false}},
      {"ERR_clear_error", {0, false}},
      {"ERR_get_error", {0, false}},
      {"ERR_peek_last_error", {0, false}},
      {"ERR_print_errors_fp", {1, false}},
      {"ERR_reason_error_string", {1, false}},
      {"OPENSSL_init_crypto", {2, false}},
      {"OPENSSL_init_ssl", {2, false}},
      {"access", {2, false}},
      {"accept", {3, false}},
      {"accept4", {4, false}},
      {"alarm", {1, false}},
      {"arc4random", {0, false}},
      {"arc4random_buf", {2, false}},
      {"bind", {3, false}},
      {"calloc", {2, false}},
      {"chdir", {1, false}},
      {"chmod", {2, false}},
      {"chown", {3, false}},
      {"chroot", {1, false}},
      {"clock_gettime", {2, false}},
      {"clock_getres", {2, false}},
      {"close", {1, false}},
      {"closedir", {1, false}},
      {"closelog", {0, false}},
      {"cfmakeraw", {1, false}},
      {"connect", {3, false}},
      {"dcgettext", {3, false}},
      {"dirfd", {1, false}},
      {"dlclose", {1, false}},
      {"dlerror", {0, false}},
      {"dlopen", {2, false}},
      {"dlsym", {2, false}},
      {"dup", {1, false}},
      {"dup2", {2, false}},
      {"dup3", {3, false}},
      {"endutxent", {0, false}},
      {"epoll_create", {1, false}},
      {"epoll_create1", {1, false}},
      {"epoll_ctl", {4, false}},
      {"epoll_pwait", {5, false}},
      {"epoll_wait", {4, false}},
      {"execv", {2, false}},
      {"execvp", {2, false}},
      {"eventfd", {2, false}},
      {"event_add", {2, false}},
      {"event_base_free", {1, false}},
      {"event_base_loop", {2, false}},
      {"event_base_loopexit", {2, false}},
      {"event_base_new_with_config", {1, false}},
      {"event_base_set", {2, false}},
      {"event_config_free", {1, false}},
      {"event_config_new", {0, false}},
      {"event_config_set_flag", {2, false}},
      {"event_del", {1, false}},
      {"event_get_version", {0, false}},
      {"event_initialized", {1, false}},
      {"event_once", {5, false}},
      {"event_set", {5, false}},
      {"exit", {1, false, true}},
      {"fclose", {1, false}},
      {"fcntl", {2, true}},
      {"fcntl64", {2, true}},
      {"fdatasync", {1, false}},
      {"fdopen", {2, false}},
      {"fflush", {1, false}},
      {"fgets", {3, false}},
      {"fgetc", {1, false}},
      {"fileno", {1, false}},
      {"fopen", {2, false}},
      {"fopen64", {2, false}},
      {"fprintf", {2, true}},
      {"fputc", {2, false}},
      {"fputs", {2, false}},
      {"fread", {4, false}},
      {"free", {1, false}},
      {"freeaddrinfo", {1, false}},
      {"freeifaddrs", {1, false}},
      {"av_freep", {1, false}},
      {"fchmod", {2, false}},
      {"fchown", {3, false}},
      {"fscanf", {2, true}},
      {"fseek", {3, false}},
      {"fstat", {2, false}},
      {"fstatfs64", {2, false}},
      {"fstat64", {2, false}},
      {"ftruncate", {2, false}},
      {"ftruncate64", {2, false}},
      {"fork", {0, false}},
      {"fsync", {1, false}},
      {"ftell", {1, false}},
      {"futimens", {2, false}},
      {"fwrite", {4, false}},
      {"gai_strerror", {1, false}},
      {"getaddrinfo", {4, false}},
      {"getcwd", {2, false}},
      {"getegid", {0, false}},
      {"getentropy", {2, false}},
      {"getenv", {1, false}},
      {"geteuid", {0, false}},
      {"getgid", {0, false}},
      {"getifaddrs", {1, false}},
      {"getgrgid", {1, false}},
      {"getgrgid_r", {5, false}},
      {"getgrnam", {1, false}},
      {"gethostname", {2, false}},
      {"getopt", {3, false}},
      {"getopt_long", {5, false}},
      {"gethostbyname", {1, false}},
      {"getloadavg", {2, false}},
      {"getpagesize", {0, false}},
      {"getpeername", {3, false}},
      {"getpid", {0, false}},
      {"getpgrp", {0, false}},
      {"getppid", {0, false}},
      {"getpriority", {2, false}},
      {"getpwnam", {1, false}},
      {"getpwuid", {1, false}},
      {"getpwuid_r", {5, false}},
      {"getrlimit", {2, false}},
      {"getrlimit64", {2, false}},
      {"getrusage", {2, false}},
      {"getsockname", {3, false}},
      {"getsockopt", {5, false}},
      {"getsubopt", {3, false}},
      {"getuid", {0, false}},
      {"gettimeofday", {2, false}},
      {"getxattr", {4, false}},
      {"getc", {1, false}},
      {"gmtime", {1, false}},
      {"gmtime_r", {2, false}},
      {"gnu_get_libc_version", {0, false}},
      {"glob64", {4, false}},
      {"globfree64", {1, false}},
      {"if_nametoindex", {1, false}},
      {"if_indextoname", {2, false}},
      {"inet_aton", {2, false}},
      {"inet_ntoa", {1, false}},
      {"inet_ntop", {4, false}},
      {"inet_pton", {3, false}},
      {"initgroups", {2, false}},
      {"inotify_add_watch", {3, false}},
      {"inotify_init1", {1, false}},
      {"inotify_rm_watch", {2, false}},
      {"ioctl", {2, true}},
      {"isatty", {1, false}},
      {"kill", {2, false}},
      {"lchown", {3, false}},
      {"link", {2, false}},
      {"listen", {2, false}},
      {"localtime", {1, false}},
      {"localtime_r", {2, false}},
      {"lstat", {2, false}},
      {"lstat64", {2, false}},
      {"lseek", {3, false}},
      {"lseek64", {3, false}},
      {"malloc", {1, false}},
      {"malloc_usable_size", {1, false}},
      {"madvise", {3, false}},
      {"memcmp", {3, false}},
      {"memcpy", {3, false}},
      {"memchr", {3, false}},
      {"memmove", {3, false}},
      {"mempcpy", {3, false}},
      {"memset", {3, false}},
      {"mkdir", {2, false}},
      {"mkdtemp", {1, false}},
      {"mkostemp64", {2, false}},
      {"mkstemp64", {1, false}},
      {"mlockall", {1, false}},
      {"mmap64", {6, false}},
      {"mktime", {1, false}},
      {"mprotect", {3, false}},
      {"msync", {3, false}},
      {"munmap", {2, false}},
      {"nanosleep", {2, false}},
      {"nettle_arcfour_crypt", {4, false}},
      {"nettle_arcfour_set_key", {3, false}},
      {"nettle_knuth_lfib_get", {1, false}},
      {"nettle_knuth_lfib_init", {2, false}},
      {"nettle_sha256_digest", {3, false}},
      {"nettle_sha256_init", {1, false}},
      {"nettle_sha256_update", {3, false}},
      {"nettle_yarrow256_init", {3, false}},
      {"nl_langinfo", {1, false}},
      {"open", {2, true}},
      {"open64", {2, true}},
      {"openlog", {3, false}},
      {"opendir", {1, false}},
      {"pathconf", {2, false}},
      {"pcre2_code_free_8", {1, false}},
      {"pcre2_get_error_message_8", {3, false}},
      {"pcre2_get_ovector_pointer_8", {1, false}},
      {"pcre2_jit_compile_8", {2, false}},
      {"pcre2_match_data_create_8", {2, false}},
      {"pcre2_match_data_create_from_pattern_8", {2, false}},
      {"pcre2_match_data_free_8", {1, false}},
      {"pcre2_pattern_info_8", {3, false}},
      {"perror", {1, false}},
      {"pipe", {1, false}},
      {"pipe2", {2, false}},
      {"posix_memalign", {3, false}},
      {"posix_spawn_file_actions_destroy", {1, false}},
      {"posix_spawn_file_actions_addclosefrom_np", {2, false}},
      {"posix_spawn_file_actions_adddup2", {3, false}},
      {"posix_spawn_file_actions_addfchdir_np", {2, false}},
      {"posix_spawn_file_actions_init", {1, false}},
      {"posix_spawnattr_destroy", {1, false}},
      {"posix_spawnattr_init", {1, false}},
      {"posix_spawnattr_setflags", {2, false}},
      {"posix_spawnattr_setsigdefault", {2, false}},
      {"posix_spawnattr_setsigmask", {2, false}},
      {"poll", {3, false}},
      {"printf", {1, true}},
      {"prctl", {1, true}},
      {"pread64", {4, false}},
      {"pread", {4, false}},
      {"preadv64", {4, false}},
      {"preadv", {4, false}},
      {"preadv64v2", {5, false}},
      {"av_packet_free", {1, false}},
      {"pwrite", {4, false}},
      {"pwrite64", {4, false}},
      {"pwritev64", {4, false}},
      {"pthread_attr_destroy", {1, false}},
      {"pthread_attr_init", {1, false}},
      {"pthread_attr_setstacksize", {2, false}},
      {"pthread_barrier_destroy", {1, false}},
      {"pthread_barrier_init", {3, false}},
      {"pthread_barrier_wait", {1, false}},
      {"pthread_cond_signal", {1, false}},
      {"pthread_cond_broadcast", {1, false}},
      {"pthread_cond_destroy", {1, false}},
      {"pthread_cond_init", {2, false}},
      {"pthread_cond_timedwait", {3, false}},
      {"pthread_cond_wait", {2, false}},
      {"pthread_condattr_destroy", {1, false}},
      {"pthread_condattr_init", {1, false}},
      {"pthread_condattr_setclock", {2, false}},
      {"pthread_create", {4, false}},
      {"pthread_getaffinity_np", {3, false}},
      {"pthread_getschedparam", {3, false}},
      {"pthread_getspecific", {1, false}},
      {"pthread_join", {2, false}},
      {"pthread_key_create", {2, false}},
      {"pthread_key_delete", {1, false}},
      {"pthread_mutex_destroy", {1, false}},
      {"pthread_mutex_init", {2, false}},
      {"pthread_mutex_lock", {1, false}},
      {"pthread_mutex_trylock", {1, false}},
      {"pthread_mutex_unlock", {1, false}},
      {"pthread_mutexattr_destroy", {1, false}},
      {"pthread_mutexattr_init", {1, false}},
      {"pthread_mutexattr_settype", {2, false}},
      {"pthread_once", {2, false}},
      {"pthread_rwlock_destroy", {1, false}},
      {"pthread_rwlock_init", {2, false}},
      {"pthread_rwlock_rdlock", {1, false}},
      {"pthread_rwlock_tryrdlock", {1, false}},
      {"pthread_rwlock_trywrlock", {1, false}},
      {"pthread_rwlock_unlock", {1, false}},
      {"pthread_rwlock_wrlock", {1, false}},
      {"pthread_self", {0, false}},
      {"pthread_setaffinity_np", {3, false}},
      {"pthread_setschedparam", {3, false}},
      {"pthread_setname_np", {2, false}},
      {"pthread_setspecific", {2, false}},
      {"pthread_sigmask", {3, false}},
      {"putc", {2, false}},
      {"putchar", {1, false}},
      {"putenv", {1, false}},
      {"puts", {1, false}},
      {"pututxline", {1, false}},
      {"qsort", {4, false}},
      {"raise", {1, false}},
      {"rand", {0, false}},
      {"random", {0, false}},
      {"read", {3, false}},
      {"readv", {3, false}},
      {"readdir", {1, false}},
      {"readdir64", {1, false}},
      {"readlink", {3, false}},
      {"re_comp", {1, false}},
      {"re_exec", {1, false}},
      {"realloc", {2, false}},
      {"realpath", {2, false}},
      {"recvmmsg", {5, false}},
      {"recv", {4, false}},
      {"recvmsg", {3, false}},
      {"rename", {2, false}},
      {"rmdir", {1, false}},
      {"sched_get_priority_max", {1, false}},
      {"sched_get_priority_min", {1, false}},
      {"sched_getaffinity", {3, false}},
      {"sched_getcpu", {0, false}},
      {"sched_yield", {0, false}},
      {"sasl_dispose", {1, false}},
      {"sasl_server_init", {2, false}},
      {"sasl_server_step", {5, false}},
      {"scandir64", {4, false}},
      {"select", {5, false}},
      {"sem_destroy", {1, false}},
      {"sem_init", {3, false}},
      {"sem_post", {1, false}},
      {"sem_trywait", {1, false}},
      {"sem_wait", {1, false}},
      {"send", {4, false}},
      {"sendfile", {4, false}},
      {"sendfile64", {4, false}},
      {"sendmmsg", {4, false}},
      {"sendmsg", {3, false}},
      {"av_strerror", {3, false}},
      {"SSL_accept", {1, false}},
      {"SSL_clear", {1, false}},
      {"SSL_connect", {1, false}},
      {"SSL_CTX_check_private_key", {1, false}},
      {"SSL_CTX_load_verify_locations", {3, false}},
      {"SSL_CTX_new", {1, false}},
      {"SSL_CTX_sess_set_new_cb", {2, false}},
      {"SSL_CTX_set_cipher_list", {2, false}},
      {"SSL_CTX_set_ciphersuites", {2, false}},
      {"SSL_CTX_set_client_CA_list", {2, false}},
      {"SSL_CTX_set_default_verify_paths", {1, false}},
      {"SSL_CTX_set_options", {2, false}},
      {"SSL_CTX_set_session_id_context", {3, false}},
      {"SSL_CTX_set_verify", {3, false}},
      {"SSL_CTX_set_verify_depth", {2, false}},
      {"SSL_CTX_use_certificate_chain_file", {2, false}},
      {"SSL_CTX_use_PrivateKey_file", {3, false}},
      {"SSL_CTX_free", {1, false}},
      {"setenv", {3, false}},
      {"setgid", {1, false}},
      {"setgroups", {2, false}},
      {"setlocale", {2, false}},
      {"setpriority", {3, false}},
      {"setregid", {2, false}},
      {"setreuid", {2, false}},
      {"setrlimit", {2, false}},
      {"setrlimit64", {2, false}},
      {"setbuf", {2, false}},
      {"setsid", {0, false}},
      {"setsockopt", {5, false}},
      {"setuid", {1, false}},
      {"setutxent", {0, false}},
      {"shutdown", {2, false}},
      {"sigaction", {3, false}},
      {"sigaddset", {2, false}},
      {"sigdelset", {2, false}},
      {"sigemptyset", {1, false}},
      {"sigfillset", {1, false}},
      {"signal", {2, false}},
      {"sigprocmask", {3, false}},
      {"snprintf", {3, true}},
      {"av_usleep", {1, false}},
      {"socket", {3, false}},
      {"socketpair", {4, false}},
      {"SSL_free", {1, false}},
      {"SSL_get_error", {2, false}},
      {"SSL_load_client_CA_file", {1, false}},
      {"SSL_new", {1, false}},
      {"SSL_pending", {1, false}},
      {"SSL_read", {3, false}},
      {"SSL_set_connect_state", {1, false}},
      {"SSL_set_fd", {2, false}},
      {"SSL_set_info_callback", {2, false}},
      {"SSL_shutdown", {1, false}},
      {"SSL_write", {3, false}},
      {"sleep", {1, false}},
      {"srandom", {1, false}},
      {"srand", {1, false}},
      {"sscanf", {2, true}},
      {"stat", {2, false}},
      {"stat64", {2, false}},
      {"statfs64", {2, false}},
      {"strcasecmp", {2, false}},
      {"strcat", {2, false}},
      {"strchr", {2, false}},
      {"strcmp", {2, false}},
      {"strcpy", {2, false}},
      {"strcspn", {2, false}},
      {"strdup", {1, false}},
      {"strerror", {1, false}},
      {"strerror_r", {3, false}},
      {"strftime", {4, false}},
      {"strlen", {1, false}},
      {"strlcat", {3, false}},
      {"strlcpy", {3, false}},
      {"strncasecmp", {3, false}},
      {"strndup", {2, false}},
      {"strnlen", {2, false}},
      {"strncmp", {3, false}},
      {"strncpy", {3, false}},
      {"strrchr", {2, false}},
      {"strsep", {2, false}},
      {"strstr", {2, false}},
      {"strtok_r", {3, false}},
      {"strtol", {3, false}},
      {"strtoll", {3, false}},
      {"strtoul", {3, false}},
      {"strtoull", {3, false}},
      {"syscall", {1, true}},
      {"sysinfo", {1, false}},
      {"sysconf", {1, false}},
      {"symlink", {2, false}},
      {"tcgetattr", {2, false}},
      {"tcsetattr", {3, false}},
      {"time", {1, false}},
      {"timegm", {1, false}},
      {"TLS_client_method", {0, false}},
      {"TLS_server_method", {0, false}},
      {"ttyname_r", {3, false}},
      {"tzset", {0, false}},
      {"umask", {1, false}},
      {"uname", {1, false}},
      {"unlink", {1, false}},
      {"unsetenv", {1, false}},
      {"updwtmpx", {2, false}},
      {"utime", {2, false}},
      {"utimensat", {4, false}},
      {"usleep", {1, false}},
      {"wait", {1, false}},
      {"waitpid", {3, false}},
      {"write", {3, false}},
      {"writev", {3, false}},
  };
  return prototypes;
}

bool isKnownNoReturnExternal(const llvm::Function &function) {
  if (!function.isDeclaration()) {
    return false;
  }
  if (function.hasFnAttribute(llvm::Attribute::NoReturn)) {
    return true;
  }
  auto knownIt = knownExternalPrototypes().find(function.getName());
  return knownIt != knownExternalPrototypes().end() && knownIt->second.NoReturn;
}

void truncateKnownNoReturnExternalCalls(llvm::Module &module) {
  std::vector<std::pair<llvm::Instruction *, llvm::Function *>> work;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    for (llvm::BasicBlock &block : function) {
      for (llvm::Instruction &inst : block) {
        auto *call = llvm::dyn_cast<llvm::CallInst>(&inst);
        if (call == nullptr || call->getNextNode() == nullptr) {
          continue;
        }
        llvm::Function *callee = call->getCalledFunction();
        if (callee == nullptr || !isKnownNoReturnExternal(*callee)) {
          continue;
        }
        work.push_back({call->getNextNode(), &function});
        break;
      }
    }
  }

  std::set<llvm::Function *> changedFunctions;
  for (auto [truncatePoint, function] : work) {
    llvm::changeToUnreachable(truncatePoint);
    changedFunctions.insert(function);
  }
  for (llvm::Function *function : changedFunctions) {
    llvm::removeUnreachableBlocks(*function);
  }
}

void eraseUnusedSummaryHelperDeclarations(llvm::Module &module) {
  std::vector<llvm::Function *> deadHelpers;
  for (llvm::Function &function : module) {
    if (function.isDeclaration() && function.use_empty() &&
        function.getName().starts_with("notdec.register.summary_")) {
      deadHelpers.push_back(&function);
    }
  }
  for (llvm::Function *function : deadHelpers) {
    function->eraseFromParent();
  }
}

std::optional<std::string> mdField(const llvm::MDNode *node,
                                   llvm::StringRef key) {
  if (node == nullptr) {
    return std::nullopt;
  }
  std::string prefix = (key + "=").str();
  for (const llvm::MDOperand &operand : node->operands()) {
    auto *text = llvm::dyn_cast_or_null<llvm::MDString>(operand.get());
    if (text == nullptr) {
      continue;
    }
    llvm::StringRef value = text->getString();
    if (value.starts_with(prefix)) {
      return value.drop_front(prefix.size()).str();
    }
  }
  return std::nullopt;
}

std::string unitName(const llvm::GlobalVariable &global) {
  if (auto name = mdField(global.getMetadata("notdec.register"), "name")) {
    if (!name->empty()) {
      return *name;
    }
  }
  return global.getName().str();
}

uint64_t unitOffset(const llvm::GlobalVariable &global) {
  if (auto offset = mdField(global.getMetadata("notdec.register"), "offset")) {
    uint64_t value = 0;
    if (!llvm::StringRef(*offset).getAsInteger(10, value)) {
      return value;
    }
  }
  return 0;
}

uint64_t unitSize(const llvm::GlobalVariable &global) {
  if (auto size = mdField(global.getMetadata("notdec.register"), "size")) {
    uint64_t value = 0;
    if (!llvm::StringRef(*size).getAsInteger(10, value)) {
      return value;
    }
  }
  return 0;
}

std::string unitSpace(const llvm::GlobalVariable &global) {
  if (auto space = mdField(global.getMetadata("notdec.register"), "space")) {
    return *space;
  }
  return "";
}

std::map<llvm::GlobalVariable *, RegisterUnit>
collectRegisterUnits(llvm::Module &module) {
  std::map<llvm::GlobalVariable *, RegisterUnit> units;
  for (llvm::GlobalVariable &global : module.globals()) {
    if (global.getMetadata("notdec.register") == nullptr) {
      continue;
    }
    RegisterUnit unit;
    unit.Global = &global;
    unit.Space = unitSpace(global);
    unit.Name = unitName(global);
    unit.Offset = unitOffset(global);
    unit.Size = unitSize(global);
    units.emplace(&global, std::move(unit));
  }
  return units;
}

RegisterAccess
registerLoad(llvm::LoadInst &load,
             const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      load.getPointerOperand()->stripPointerCasts());
  if (global == nullptr) {
    return {};
  }
  auto it = units.find(global);
  if (it == units.end()) {
    return {};
  }
  if (load.getMetadata("notdec.register.access") == nullptr &&
      global->getMetadata("notdec.register") == nullptr) {
    return {};
  }
  return RegisterAccess{&it->second, true,
                        load.getType() == global->getValueType()};
}

RegisterAccess
registerStore(llvm::StoreInst &store,
              const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
      store.getPointerOperand()->stripPointerCasts());
  if (global == nullptr) {
    return {};
  }
  auto it = units.find(global);
  if (it == units.end()) {
    return {};
  }
  if (store.getMetadata("notdec.register.access") == nullptr &&
      global->getMetadata("notdec.register") == nullptr) {
    return {};
  }
  return RegisterAccess{&it->second, true,
                        store.getValueOperand()->getType() ==
                            global->getValueType()};
}

bool isNotDecRegisterHelperCall(const llvm::CallBase &call) {
  llvm::Function *callee = call.getCalledFunction();
  return callee != nullptr && callee->getName().starts_with("notdec.register.");
}

bool isAnalyzableCall(const llvm::CallBase &call) {
  if (isNotDecRegisterHelperCall(call)) {
    return false;
  }
  llvm::Function *callee = call.getCalledFunction();
  return callee == nullptr || !callee->isIntrinsic();
}

bool isKeepHighPartialLoadUse(llvm::LoadInst &load,
                              llvm::GlobalVariable *global) {
  // RegisterStorage lowers partial writes by reading the whole backing
  // register, preserving the lanes outside the write mask, and storing the
  // whole register back.  This load is not a real liveness use when it only
  // feeds that keep-high expression.
  if (load.getPointerOperand()->stripPointerCasts() != global ||
      load.user_empty()) {
    return false;
  }
  for (llvm::User *loadUser : load.users()) {
    auto *andOp = llvm::dyn_cast<llvm::Operator>(loadUser);
    if (andOp == nullptr || andOp->getOpcode() != llvm::Instruction::And) {
      return false;
    }
    bool hasStore = false;
    for (llvm::User *andUser : andOp->users()) {
      auto *orOp = llvm::dyn_cast<llvm::Operator>(andUser);
      if (orOp == nullptr || orOp->getOpcode() != llvm::Instruction::Or) {
        return false;
      }
      for (llvm::User *orUser : orOp->users()) {
        auto *store = llvm::dyn_cast<llvm::StoreInst>(orUser);
        if (store == nullptr ||
            store->getPointerOperand()->stripPointerCasts() != global ||
            store->getValueOperand() != orOp) {
          return false;
        }
        hasStore = true;
      }
    }
    if (!hasStore) {
      return false;
    }
  }
  return true;
}

std::string storageUnitName(
    const NativeAbiStorage &storage,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  // ABI records may mention partial names such as XMM0_Qa while lifting keeps
  // only the largest overlapping register global such as ZMM0.
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Name == storage.Name) {
      return unit.Name;
    }
  }
  for (llvm::StringRef prefix : {llvm::StringRef("XMM"),
                                llvm::StringRef("YMM")}) {
    llvm::StringRef name(storage.Name);
    if (!name.starts_with(prefix)) {
      continue;
    }
    llvm::StringRef rest = name.drop_front(prefix.size());
    size_t digits = 0;
    while (digits < rest.size() && rest[digits] >= '0' &&
           rest[digits] <= '9') {
      ++digits;
    }
    if (digits == 0) {
      continue;
    }
    std::string candidate = ("ZMM" + rest.take_front(digits)).str();
    for (const auto &[global, unit] : units) {
      (void)global;
      if (unit.Name == candidate) {
        return unit.Name;
      }
    }
  }
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Space == storage.Space && unit.Offset <= storage.Offset &&
        storage.Offset < unit.Offset + unit.Size) {
      return unit.Name;
    }
  }
  return storage.Name;
}

void pushUnique(std::vector<std::string> &items, const std::string &item) {
  if (std::find(items.begin(), items.end(), item) == items.end()) {
    items.push_back(item);
  }
}

AbiFacts collectAbiFacts(
    const llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units) {
  AbiFacts facts;
  std::optional<NativeAbiSpec> abi = readNativeAbiMetadata(module);
  if (!abi) {
    return facts;
  }
  for (const NativeAbiParamEntry &entry : abi->Inputs) {
    if (entry.Storage.Kind == NativeAbiStorageKind::Register &&
        !entry.Storage.Name.empty()) {
      std::string name = storageUnitName(entry.Storage, units);
      facts.Inputs.insert(name);
      if (entry.MetaType == "float") {
        continue;
      }
      facts.InternalParamRegisters.insert(name);
      pushUnique(facts.InputsInOrder, name);
    }
  }
  for (const NativeAbiParamEntry &entry : abi->Outputs) {
    if (entry.Storage.Kind == NativeAbiStorageKind::Register &&
        !entry.Storage.Name.empty()) {
      std::string name = storageUnitName(entry.Storage, units);
      facts.Outputs.insert(name);
      if (entry.MetaType == "float") {
        continue;
      }
      facts.InternalReturnRegisters.insert(name);
      pushUnique(facts.OutputsInOrder, name);
    }
  }
  for (const NativeAbiEffect &effect : abi->Effects) {
    if (effect.Storage.Kind != NativeAbiStorageKind::Register ||
        effect.Storage.Name.empty()) {
      continue;
    }
    if (effect.Kind == NativeAbiEffectKind::Unaffected) {
      std::string name = storageUnitName(effect.Storage, units);
      facts.Unaffected.insert(name);
      facts.InternalParamRegisters.insert(name);
      facts.InternalReturnRegisters.insert(name);
    } else if (effect.Kind == NativeAbiEffectKind::KilledByCall) {
      facts.KilledByCall.insert(storageUnitName(effect.Storage, units));
    }
  }
  return facts;
}

std::map<llvm::Function *, FunctionSummaryFacts>
summaryFactsByFunction(const NativeRegisterSummary &summary,
                       llvm::Module &module) {
  std::map<llvm::Function *, FunctionSummaryFacts> result;
  for (const NativeRegisterSummaryFunction &functionSummary :
       summary.Functions) {
    llvm::Function *function = module.getFunction(functionSummary.FunctionName);
    if (function == nullptr) {
      continue;
    }
    FunctionSummaryFacts facts;
    for (const NativeRegisterSummaryRegister &reg : functionSummary.Registers) {
      facts.Registers.emplace(
          reg.Name, SummaryRegisterFact{reg.ReadEntry, reg.MayEntry,
                                        reg.MayNonEntry, reg.ExitDemand});
    }
    result.emplace(function, std::move(facts));
  }
  return result;
}

std::string typeSuffix(llvm::Type &type) {
  if (auto *integerType = llvm::dyn_cast<llvm::IntegerType>(&type)) {
    return "i" + std::to_string(integerType->getBitWidth());
  }
  return "value";
}

const RegisterUnit *
unitByName(const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
           llvm::StringRef name) {
  for (const auto &[global, unit] : units) {
    (void)global;
    if (unit.Name == name) {
      return &unit;
    }
  }
  return nullptr;
}

llvm::Type *singleReturnType(const SignatureShape &shape) {
  if (shape.Returns.empty()) {
    return nullptr;
  }
  return shape.Returns.front()->Global->getValueType();
}

llvm::Type *returnTypeForShape(llvm::LLVMContext &context,
                               const SignatureShape &shape) {
  if (shape.Returns.empty()) {
    return llvm::Type::getVoidTy(context);
  }
  if (shape.Returns.size() == 1) {
    return singleReturnType(shape);
  }
  std::vector<llvm::Type *> fields;
  fields.reserve(shape.Returns.size());
  for (const RegisterUnit *unit : shape.Returns) {
    fields.push_back(unit->Global->getValueType());
  }
  return llvm::StructType::get(context, fields);
}

llvm::FunctionType *functionTypeForShape(llvm::LLVMContext &context,
                                         const SignatureShape &shape) {
  std::vector<llvm::Type *> params;
  params.reserve(shape.Params.size());
  for (const RegisterUnit *unit : shape.Params) {
    params.push_back(unit->Global->getValueType());
  }
  return llvm::FunctionType::get(returnTypeForShape(context, shape), params,
                                 shape.VarArg);
}

SignatureShape shapeForInternalFunction(
    llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts,
    const AbiFacts &abi) {
  SignatureShape shape;
  auto factsIt = summaryFacts.find(&function);
  if (factsIt == summaryFacts.end()) {
    return shape;
  }

  std::vector<const RegisterUnit *> orderedUnits;
  orderedUnits.reserve(units.size());
  for (const auto &[global, unit] : units) {
    (void)global;
    orderedUnits.push_back(&unit);
  }
  std::sort(orderedUnits.begin(), orderedUnits.end(),
            [](const RegisterUnit *lhs, const RegisterUnit *rhs) {
              if (lhs->Offset != rhs->Offset) {
                return lhs->Offset < rhs->Offset;
              }
              return lhs->Name < rhs->Name;
            });

  // Internal native functions can be compiled with interprocedural register
  // allocation, so their real interface is not limited to the external ABI
  // argument and return registers.  Stay within ABI-described general-purpose
  // registers, but let summary facts decide which of them are real inputs and
  // demanded outputs.
  for (const RegisterUnit *unit : orderedUnits) {
    if (abi.InternalParamRegisters.count(unit->Name) == 0) {
      continue;
    }
    auto regIt = factsIt->second.Registers.find(unit->Name);
    if (regIt != factsIt->second.Registers.end() && regIt->second.ReadEntry) {
      shape.Params.push_back(unit);
    }
  }

  for (const RegisterUnit *unit : orderedUnits) {
    if (abi.InternalReturnRegisters.count(unit->Name) == 0) {
      continue;
    }
    auto regIt = factsIt->second.Registers.find(unit->Name);
    if (regIt != factsIt->second.Registers.end() &&
        regIt->second.MayNonEntry && regIt->second.ExitDemand) {
      shape.Returns.push_back(unit);
    }
  }
  return shape;
}

SignatureShape shapeForKnownExternal(
    llvm::Function &function,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi) {
  SignatureShape shape;
  auto knownIt = knownExternalPrototypes().find(function.getName());
  unsigned count = 0;
  if (knownIt == knownExternalPrototypes().end()) {
    count = abi.InputsInOrder.size();
  } else {
    count =
        std::min<unsigned>(knownIt->second.FixedArgs, abi.InputsInOrder.size());
    shape.VarArg = knownIt->second.VarArg;
  }
  for (unsigned index = 0; index < count; ++index) {
    const RegisterUnit *unit = unitByName(units, abi.InputsInOrder[index]);
    if (unit != nullptr) {
      shape.Params.push_back(unit);
    }
  }
  return shape;
}

void addDemandedExternalReturns(
    SignatureRewriteState &state,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const AbiFacts &abi) {
  for (const auto &[call, helpers] : state.ReturnHelpers) {
    llvm::Function *callee = call->getCalledFunction();
    if (callee == nullptr || !callee->isDeclaration()) {
      continue;
    }
    auto shapeIt = state.Shapes.find(callee);
    if (shapeIt == state.Shapes.end()) {
      continue;
    }
    if (state.CallArgs.count(call) == 0) {
      state.CallArgs.emplace(call, std::vector<CallArgStoreBinding>{});
    }
    std::optional<unsigned> maxReturnRegisters;
    auto knownIt = knownExternalPrototypes().find(callee->getName());
    if (knownIt != knownExternalPrototypes().end()) {
      maxReturnRegisters =
          knownIt->second.NoReturn ? 0 : knownIt->second.MaxReturnRegisters;
    }
    unsigned returnIndex = 0;
    for (const std::string &name : abi.OutputsInOrder) {
      if (maxReturnRegisters.has_value() &&
          returnIndex >= *maxReturnRegisters) {
        break;
      }
      if (helpers.count(name) == 0) {
        ++returnIndex;
        continue;
      }
      bool alreadyPresent = false;
      for (const RegisterUnit *unit : shapeIt->second.Returns) {
        alreadyPresent |= unit->Name == name;
      }
      if (!alreadyPresent) {
        const RegisterUnit *unit = unitByName(units, name);
        if (unit != nullptr) {
          shapeIt->second.Returns.push_back(unit);
        }
      }
      ++returnIndex;
    }
  }
}

std::map<llvm::Function *, SignatureShape> buildInitialSignatureShapes(
    llvm::Module &module,
    const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
    const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts,
    const AbiFacts &abi) {
  std::map<llvm::Function *, SignatureShape> shapes;
  for (llvm::Function &function : module) {
    if (function.isIntrinsic() ||
        function.getName().starts_with("notdec.register.")) {
      continue;
    }
    SignatureShape shape =
        function.isDeclaration()
            ? shapeForKnownExternal(function, units, abi)
            : shapeForInternalFunction(function, units, summaryFacts, abi);
    if (!shape.Params.empty() || !shape.Returns.empty() || shape.VarArg) {
      shapes.emplace(&function, std::move(shape));
    }
  }
  return shapes;
}

class FunctionBuilder {
public:
  FunctionBuilder(
      llvm::Function &function,
      const std::map<llvm::GlobalVariable *, RegisterUnit> &units,
      const std::map<llvm::Function *, FunctionSummaryFacts> &summaryFacts,
      const AbiFacts &abiFacts, const NativeRegisterSummarySSAOptions &options,
      NativeRegisterSummarySSAFunctionSummary &summary,
      SignatureRewriteState &signatureState)
      : Function(function), Units(units), SummaryFacts(summaryFacts),
        Abi(abiFacts), Options(options), Summary(summary),
        SignatureState(signatureState) {}

  void run() {
    Summary.FunctionName = Function.getName().str();
    collectAccesses();
    if (Options.EnableRewrite) {
      rewriteLoads();
      collectSignatureCallArgs();
      finalizePendingPhis();
      collectFunctionReturnValues();
      if (Options.EnableResidueRemoval) {
        removeDeadReplacedLoads();
        removeDeadStoresByLiveness();
      }
      eraseDeadPhis();
    }
    if (Options.AttachMetadata) {
      attachMetadata();
    }
  }

  void removeDeadStoresAfterSignatureRewrite() {
    Summary.FunctionName = Function.getName().str();
    PostSignatureCleanup = true;
    removeDeadStoresByLiveness();
  }

private:
  llvm::Function &Function;
  const std::map<llvm::GlobalVariable *, RegisterUnit> &Units;
  const std::map<llvm::Function *, FunctionSummaryFacts> &SummaryFacts;
  const AbiFacts &Abi;
  const NativeRegisterSummarySSAOptions &Options;
  NativeRegisterSummarySSAFunctionSummary &Summary;
  SignatureRewriteState &SignatureState;
  std::vector<llvm::LoadInst *> Loads;
  std::vector<llvm::LoadInst *> ReplacedLoads;
  std::map<BlockRegKey, llvm::Value *> EntryValue;
  std::map<BlockRegKey, llvm::Value *> ExitValue;
  std::map<BlockRegKey, llvm::PHINode *> PendingPhi;
  std::set<BlockRegKey> ResolvingEntry;
  std::map<llvm::Value *, llvm::Value *> Replacement;
  std::set<llvm::PHINode *> DeadPhis;
  std::map<llvm::GlobalVariable *, llvm::LoadInst *> EntryInputs;
  std::map<CallValueKey, llvm::Value *> CallValues;
  bool PostSignatureCleanup = false;

  void collectAccesses() {
    for (llvm::BasicBlock &block : Function) {
      for (llvm::Instruction &inst : block) {
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
          RegisterAccess access = registerLoad(*load, Units);
          if (access.Unit != nullptr) {
            ++Summary.LoadsSeen;
            if (access.IsStorageValue &&
                load->getMetadata("notdec.register.summary_ssa.entry") ==
                    nullptr) {
              Loads.push_back(load);
            }
          }
          continue;
        }
        if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
          RegisterAccess access = registerStore(*store, Units);
          if (access.Unit != nullptr) {
            ++Summary.StoresSeen;
          }
        }
      }
    }
  }

  void rewriteLoads() {
    for (llvm::LoadInst *load : Loads) {
      RegisterAccess access = registerLoad(*load, Units);
      if (access.Unit == nullptr || !access.IsStorageValue) {
        continue;
      }
      llvm::Value *value =
          readValueBefore(*load->getParent(), *access.Unit, load);
      value = resolve(value);
      if (value == nullptr || value == load ||
          value->getType() != load->getType()) {
        continue;
      }
      Replacement[load] = value;
      load->replaceAllUsesWith(value);
      load->setMetadata("notdec.register.summary_ssa.replaced",
                        markerNode("true"));
      ReplacedLoads.push_back(load);
      ++Summary.LoadsReplaced;
    }
  }

  void removeDeadReplacedLoads() {
    for (llvm::LoadInst *load : ReplacedLoads) {
      if (load->getParent() == nullptr || !load->use_empty()) {
        continue;
      }
      if (isRecordedCallArgValue(load)) {
        continue;
      }
      load->eraseFromParent();
      ++Summary.DeadLoadsRemoved;
    }
  }

  bool isRecordedCallArgStore(llvm::StoreInst *store) const {
    return SignatureState.StoresToErase.count(store) != 0;
  }

  bool isRecordedCallArgValue(llvm::Value *value) const {
    for (const auto &[call, bindings] : SignatureState.CallArgs) {
      (void)call;
      for (const CallArgStoreBinding &binding : bindings) {
        if (binding.Value == value) {
          return true;
        }
      }
    }
    return false;
  }

  void removeDeadStoresByLiveness() {
    std::map<llvm::BasicBlock *, std::set<llvm::GlobalVariable *>> liveIn;
    std::map<llvm::BasicBlock *, std::set<llvm::GlobalVariable *>> liveOut;
    std::vector<llvm::BasicBlock *> blocks;
    for (llvm::BasicBlock &block : Function) {
      blocks.push_back(&block);
    }

    bool changed = true;
    while (changed) {
      changed = false;
      for (auto blockIt = blocks.rbegin(); blockIt != blocks.rend();
           ++blockIt) {
        llvm::BasicBlock &block = **blockIt;
        std::set<llvm::GlobalVariable *> out;
        for (llvm::BasicBlock *succ : llvm::successors(&block)) {
          auto succLive = liveIn.find(succ);
          if (succLive != liveIn.end()) {
            out.insert(succLive->second.begin(), succLive->second.end());
          }
        }
        // After signature rewrite, explicit function returns carry these
        // values.  Register globals should no longer stay live just because a
        // summary return register exists.
        if (!PostSignatureCleanup && llvm::succ_empty(&block)) {
          addExitLiveRegisters(out);
        }

        std::set<llvm::GlobalVariable *> in = transferBlockLiveness(block, out);
        changed |= liveOut[&block] != out || liveIn[&block] != in;
        liveOut[&block] = std::move(out);
        liveIn[&block] = std::move(in);
      }
    }

    for (llvm::BasicBlock &block : Function) {
      auto outIt = liveOut.find(&block);
      std::set<llvm::GlobalVariable *> live =
          outIt == liveOut.end() ? std::set<llvm::GlobalVariable *>{}
                                 : outIt->second;
      eraseDeadStoresInBlock(block, live);
    }
  }

  std::set<llvm::GlobalVariable *>
  transferBlockLiveness(llvm::BasicBlock &block,
                        std::set<llvm::GlobalVariable *> live) {
    for (auto it = block.rbegin(); it != block.rend(); ++it) {
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        transferStoreLiveness(*store, live);
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        transferLoadLiveness(*load, live);
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        transferCallLiveness(*call, live);
        continue;
      }
    }
    return live;
  }

  void eraseDeadStoresInBlock(llvm::BasicBlock &block,
                              std::set<llvm::GlobalVariable *> live) {
    std::vector<llvm::StoreInst *> deadStores;
    for (auto it = block.rbegin(); it != block.rend(); ++it) {
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.IsStorageValue &&
            live.count(access.Unit->Global) == 0) {
          deadStores.push_back(store);
        }
        transferStoreLiveness(*store, live);
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        transferLoadLiveness(*load, live);
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        transferCallLiveness(*call, live);
        continue;
      }
    }
    for (llvm::StoreInst *store : deadStores) {
      llvm::Value *storedValue = store->getValueOperand();
      bool keepStoredValue =
          !PostSignatureCleanup || isRecordedCallArgStore(store) ||
          isRecordedCallArgValue(storedValue);
      store->eraseFromParent();
      if (!keepStoredValue) {
        if (auto *storedInst = llvm::dyn_cast<llvm::Instruction>(storedValue)) {
          llvm::RecursivelyDeleteTriviallyDeadInstructions(storedInst);
        }
      }
      ++Summary.DeadStoresRemoved;
    }
  }

  void transferStoreLiveness(llvm::StoreInst &store,
                             std::set<llvm::GlobalVariable *> &live) const {
    RegisterAccess access = registerStore(store, Units);
    if (access.Unit != nullptr && access.IsStorageValue) {
      live.erase(access.Unit->Global);
    }
  }

  void transferLoadLiveness(llvm::LoadInst &load,
                            std::set<llvm::GlobalVariable *> &live) const {
    RegisterAccess access = registerLoad(load, Units);
    if (access.Unit != nullptr && access.IsStorageValue) {
      if (isKeepHighPartialLoadUse(load, access.Unit->Global)) {
        return;
      }
      // Entry/replaced loads are SummarySSA scaffolding.  After signature
      // rewrite, only still-raw register loads require keeping global stores.
      if (PostSignatureCleanup &&
          (load.getMetadata("notdec.register.summary_ssa.entry") != nullptr ||
           load.getMetadata("notdec.register.summary_ssa.replaced") !=
               nullptr)) {
        return;
      }
      live.insert(access.Unit->Global);
    }
  }

  void transferCallLiveness(llvm::CallBase &call,
                            std::set<llvm::GlobalVariable *> &live) const {
    if (!isAnalyzableCall(call)) {
      return;
    }
    for (const auto &[global, unit] : Units) {
      CallRegisterEffect effect = callEffect(call, unit);
      if (effect == CallRegisterEffect::ReturnValue ||
          effect == CallRegisterEffect::Clobber) {
        live.erase(global);
      }
      if (callReadsRegister(call, unit)) {
        live.insert(global);
      }
    }
  }

  void addExitLiveRegisters(std::set<llvm::GlobalVariable *> &live) const {
    auto functionFacts = SummaryFacts.find(&Function);
    if (functionFacts == SummaryFacts.end()) {
      return;
    }
    for (const auto &[global, unit] : Units) {
      auto regIt = functionFacts->second.Registers.find(unit.Name);
      if (regIt == functionFacts->second.Registers.end()) {
        continue;
      }
      const SummaryRegisterFact &fact = regIt->second;
      if (fact.ExitDemand && fact.MayNonEntry) {
        live.insert(global);
      }
    }
  }

  llvm::Value *readValueBefore(llvm::BasicBlock &block,
                               const RegisterUnit &unit,
                               llvm::Instruction *before) {
    for (auto it = before->getIterator(); it != block.begin();) {
      --it;
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.Unit->Global == unit.Global &&
            access.IsStorageValue) {
          return resolve(store->getValueOperand());
        }
        continue;
      }
      if (auto *call = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        if (!isAnalyzableCall(*call)) {
          continue;
        }
        CallRegisterEffect effect = callEffect(*call, unit);
        if (effect == CallRegisterEffect::Preserve) {
          ++Summary.PreservedCalls;
          continue;
        }
        if (effect == CallRegisterEffect::ReturnValue) {
          return callValue(*call, unit, "return");
        }
        if (effect == CallRegisterEffect::Clobber) {
          return callValue(*call, unit, "clobber");
        }
        ++Summary.UnknownCallEffects;
        return nullptr;
      }
    }
    return readBlockEntry(block, unit);
  }

  llvm::Value *readBlockEntry(llvm::BasicBlock &block,
                              const RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    if (auto cached = EntryValue.find(key); cached != EntryValue.end()) {
      return resolve(cached->second);
    }
    if (ResolvingEntry.count(key) != 0) {
      return ensurePhi(block, unit);
    }

    ResolvingEntry.insert(key);
    std::vector<llvm::BasicBlock *> preds(llvm::pred_begin(&block),
                                          llvm::pred_end(&block));
    llvm::Value *value = nullptr;
    if (preds.empty()) {
      value = entryInput(unit);
    } else if (preds.size() == 1) {
      value = readBlockExit(*preds.front(), unit);
      if (PendingPhi.count(key) != 0) {
        value = completePhi(block, unit);
      }
    } else {
      value = completePhi(block, unit);
    }
    ResolvingEntry.erase(key);
    EntryValue[key] = resolve(value);
    return EntryValue[key];
  }

  llvm::Value *readBlockExit(llvm::BasicBlock &block,
                             const RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    if (auto cached = ExitValue.find(key); cached != ExitValue.end()) {
      return resolve(cached->second);
    }
    llvm::Instruction *terminator = block.getTerminator();
    llvm::Value *value = terminator == nullptr
                             ? readBlockEntry(block, unit)
                             : readValueBefore(block, unit, terminator);
    ExitValue[key] = resolve(value);
    return ExitValue[key];
  }

  llvm::PHINode *ensurePhi(llvm::BasicBlock &block, const RegisterUnit &unit) {
    BlockRegKey key{&block, unit.Global};
    if (auto existing = PendingPhi.find(key); existing != PendingPhi.end()) {
      return existing->second;
    }
    llvm::IRBuilder<> builder(&block, block.getFirstNonPHIIt());
    llvm::PHINode *phi = builder.CreatePHI(unit.Global->getValueType(), 0,
                                           unit.Name + ".summary_ssa");
    phi->setMetadata("notdec.register.summary_ssa.phi", registerNode(unit));
    PendingPhi.emplace(key, phi);
    EntryValue[key] = phi;
    ++Summary.PhisCreated;
    return phi;
  }

  llvm::Value *completePhi(llvm::BasicBlock &block, const RegisterUnit &unit) {
    llvm::PHINode *phi = ensurePhi(block, unit);
    // LLVM PHI operands are edge-based. A switch can contribute the same
    // predecessor block more than once, so count per-block occurrences.
    std::map<llvm::BasicBlock *, unsigned> existingIncoming;
    for (unsigned index = 0; index < phi->getNumIncomingValues(); ++index) {
      ++existingIncoming[phi->getIncomingBlock(index)];
    }
    std::map<llvm::BasicBlock *, unsigned> requiredIncoming;
    for (llvm::BasicBlock *pred : llvm::predecessors(&block)) {
      unsigned requiredCount = ++requiredIncoming[pred];
      if (existingIncoming[pred] >= requiredCount) {
        continue;
      }
      llvm::Value *incoming = resolve(readBlockExit(*pred, unit));
      if (incoming == nullptr) {
        llvm::Instruction *terminator = pred->getTerminator();
        incoming = terminator != nullptr
                       ? frozenPoisonBefore(*terminator,
                                            unit.Global->getValueType(),
                                            unit.Name + ".unknown")
                       : llvm::UndefValue::get(unit.Global->getValueType());
      }
      phi->addIncoming(incoming, pred);
    }
    return simplifyPhi(*phi);
  }

  llvm::Value *simplifyPhi(llvm::PHINode &phi) {
    if (!isCompletePhi(phi)) {
      return &phi;
    }
    llvm::Value *same = nullptr;
    for (llvm::Value *incoming : phi.incoming_values()) {
      incoming = resolve(incoming);
      if (incoming == &phi) {
        continue;
      }
      if (same == nullptr) {
        same = incoming;
        continue;
      }
      if (same != incoming) {
        return &phi;
      }
    }
    if (same == nullptr) {
      return &phi;
    }
    Replacement[&phi] = same;
    phi.replaceAllUsesWith(same);
    DeadPhis.insert(&phi);
    ++Summary.PhisSimplified;
    return same;
  }

  bool isCompletePhi(const llvm::PHINode &phi) const {
    const llvm::BasicBlock *block = phi.getParent();
    return block != nullptr &&
           phi.getNumIncomingValues() == llvm::pred_size(block);
  }

  void finalizePendingPhis() {
    bool changed = true;
    while (changed) {
      changed = false;
      std::vector<std::pair<llvm::BasicBlock *, const RegisterUnit *>> work;
      for (const auto &[key, phi] : PendingPhi) {
        auto unitIt = Units.find(key.second);
        if (unitIt != Units.end() && DeadPhis.count(phi) == 0 &&
            !isCompletePhi(*phi)) {
          work.push_back({key.first, &unitIt->second});
        }
      }
      for (const auto &[block, unit] : work) {
        llvm::PHINode *phi = PendingPhi[{block, unit->Global}];
        unsigned before = phi->getNumIncomingValues();
        (void)completePhi(*block, *unit);
        changed |= phi->getNumIncomingValues() != before;
      }
    }
  }

  void eraseDeadPhis() {
    for (llvm::PHINode *phi : DeadPhis) {
      if (phi->use_empty()) {
        phi->eraseFromParent();
      }
    }
  }

  llvm::Value *resolve(llvm::Value *value) {
    while (value != nullptr) {
      auto it = Replacement.find(value);
      if (it == Replacement.end() || it->second == value) {
        return value;
      }
      value = it->second;
    }
    return nullptr;
  }

  llvm::LoadInst *entryInput(const RegisterUnit &unit) {
    if (auto cached = EntryInputs.find(unit.Global);
        cached != EntryInputs.end()) {
      return cached->second;
    }
    llvm::IRBuilder<> builder(&Function.getEntryBlock(),
                              Function.getEntryBlock().getFirstNonPHIIt());
    llvm::LoadInst *load = builder.CreateLoad(
        unit.Global->getValueType(), unit.Global, unit.Name + ".entry");
    load->setMetadata("notdec.register.summary_ssa.entry", registerNode(unit));
    EntryInputs.emplace(unit.Global, load);
    ++Summary.EntryInputs;
    return load;
  }

  CallRegisterEffect callEffect(const llvm::CallBase &call,
                                const RegisterUnit &unit) const {
    llvm::Function *callee = call.getCalledFunction();
    if (PostSignatureCleanup &&
        SignatureState.RewrittenCalls.count(&call) != 0) {
      return CallRegisterEffect::Unknown;
    }
    if (callee != nullptr && !callee->isDeclaration()) {
      auto fnIt = SummaryFacts.find(callee);
      if (fnIt == SummaryFacts.end()) {
        return CallRegisterEffect::Unknown;
      }
      auto regIt = fnIt->second.Registers.find(unit.Name);
      SummaryRegisterFact fact;
      if (regIt != fnIt->second.Registers.end()) {
        fact = regIt->second;
      }
      if (!fact.MayNonEntry) {
        return CallRegisterEffect::Preserve;
      }
      if (fact.ExitDemand) {
        return CallRegisterEffect::ReturnValue;
      }
      if (!fact.MayEntry) {
        return CallRegisterEffect::Clobber;
      }
      return CallRegisterEffect::Unknown;
    }

    if (Abi.Unaffected.count(unit.Name) != 0) {
      return CallRegisterEffect::Preserve;
    }
    if (Abi.Outputs.count(unit.Name) != 0) {
      return CallRegisterEffect::ReturnValue;
    }
    if (Abi.KilledByCall.count(unit.Name) != 0) {
      return CallRegisterEffect::Clobber;
    }
    return CallRegisterEffect::Unknown;
  }

  bool callReadsRegister(const llvm::CallBase &call,
                         const RegisterUnit &unit) const {
    llvm::Function *callee = call.getCalledFunction();
    if (PostSignatureCleanup) {
      return false;
    }
    if (callee != nullptr && !callee->isDeclaration()) {
      auto fnIt = SummaryFacts.find(callee);
      if (fnIt == SummaryFacts.end()) {
        return Abi.Inputs.count(unit.Name) != 0;
      }
      auto regIt = fnIt->second.Registers.find(unit.Name);
      return regIt != fnIt->second.Registers.end() && regIt->second.ReadEntry;
    }
    return Abi.Inputs.count(unit.Name) != 0;
  }

  void collectSignatureCallArgs() {
    std::vector<llvm::CallBase *> calls;
    for (llvm::Instruction &inst : llvm::instructions(Function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call == nullptr || !isAnalyzableCall(*call)) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      if (callee == nullptr || SignatureState.Shapes.count(callee) == 0) {
        continue;
      }
      calls.push_back(call);
    }

    for (llvm::CallBase *call : calls) {
      if (call->getParent() == nullptr) {
        continue;
      }
      llvm::Function *callee = call->getCalledFunction();
      const SignatureShape &shape = SignatureState.Shapes.at(callee);
      std::vector<CallArgStoreBinding> bindings =
          callArgStoreBindings(*call, shape);
      if (bindings.empty() && shape.Params.empty() && shape.Returns.empty()) {
        continue;
      }

      SignatureState.CallArgs[call] = bindings;
      for (const CallArgStoreBinding &binding : bindings) {
        if (binding.Store != nullptr) {
          SignatureState.StoresToErase.insert(binding.Store);
          ++Summary.CallArgStoresMarked;
        }
      }
    }
  }

  const RegisterUnit *unitByName(llvm::StringRef name) const {
    for (const auto &[global, unit] : Units) {
      if (unit.Name == name) {
        return &unit;
      }
    }
    return nullptr;
  }

  std::vector<CallArgStoreBinding>
  callArgStoreBindings(llvm::CallBase &call, const SignatureShape &shape) {
    std::vector<CallArgStoreBinding> bindings;
    llvm::Function *callee = call.getCalledFunction();
    bool allowEntryInputs = callee != nullptr && !callee->isDeclaration();
    unsigned argCount =
        shape.VarArg ? Abi.InputsInOrder.size() : shape.Params.size();
    for (unsigned index = 0; index < argCount; ++index) {
      const RegisterUnit *unit =
          index < shape.Params.size() ? shape.Params[index]
                                      : unitByName(Abi.InputsInOrder[index]);
      if (unit == nullptr) {
        break;
      }
      llvm::Value *value =
          resolve(readValueBefore(*call.getParent(), *unit, &call));
      if (value == nullptr ||
          value->getType() != unit->Global->getValueType()) {
        break;
      }
      llvm::StoreInst *store = findStoreBeforeCall(call, *unit, value);
      if (isEntryInputValue(value) && store == nullptr && !allowEntryInputs) {
        break;
      }
      bindings.push_back(CallArgStoreBinding{store, unit, value, index});
    }
    return bindings;
  }

  llvm::StoreInst *findStoreBeforeCall(llvm::CallBase &call,
                                       const RegisterUnit &unit,
                                       llvm::Value *value) {
    for (auto it = call.getIterator(); it != call.getParent()->begin();) {
      --it;
      llvm::Instruction &inst = *it;
      if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        RegisterAccess access = registerStore(*store, Units);
        if (access.Unit != nullptr && access.Unit->Global == unit.Global &&
            access.IsStorageValue) {
          return resolve(store->getValueOperand()) == value ? store : nullptr;
        }
        continue;
      }
      if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst)) {
        RegisterAccess access = registerLoad(*load, Units);
        if (access.Unit != nullptr && access.Unit->Global == unit.Global &&
            access.IsStorageValue) {
          if (load->getMetadata("notdec.register.summary_ssa.replaced") ==
              nullptr) {
            return nullptr;
          }
        }
        continue;
      }
      if (auto *otherCall = llvm::dyn_cast<llvm::CallBase>(&inst)) {
        llvm::Function *callee = otherCall->getCalledFunction();
        if (isAnalyzableCall(*otherCall) &&
            (callee == nullptr || !callee->isIntrinsic())) {
          return nullptr;
        }
        continue;
      }
      if (inst.mayWriteToMemory()) {
        return nullptr;
      }
    }
    return nullptr;
  }

  bool isEntryInputValue(llvm::Value *value) {
    value = resolve(value);
    for (const auto &[global, load] : EntryInputs) {
      if (resolve(load) == value) {
        return true;
      }
    }
    return false;
  }

  llvm::Value *callValue(llvm::CallBase &call, const RegisterUnit &unit,
                         llvm::StringRef kind) {
    CallValueKey key{&call, unit.Global, kind.str()};
    if (auto cached = CallValues.find(key); cached != CallValues.end()) {
      return cached->second;
    }

    llvm::Instruction *insertBefore = call.getNextNode();
    if (insertBefore == nullptr) {
      insertBefore = call.getParent()->getTerminator();
    }
    if (insertBefore == nullptr) {
      return llvm::UndefValue::get(unit.Global->getValueType());
    }
    if (kind == "return" && Abi.OutputsInOrder.end() ==
                                std::find(Abi.OutputsInOrder.begin(),
                                          Abi.OutputsInOrder.end(),
                                          unit.Name)) {
      return frozenPoisonBefore(*insertBefore, unit.Global->getValueType(),
                                unit.Name + ".return_unknown");
    }

    llvm::IRBuilder<> builder(insertBefore);
    llvm::CallInst *value = builder.CreateCall(callValueHelper(unit, kind), {},
                                               unit.Name + "." + kind.str());
    value->setMetadata("notdec.register.summary_ssa.call_value",
                       callValueNode(unit, kind, &call));
    CallValues.emplace(key, value);
    if (kind == "return") {
      SignatureState.ReturnHelpers[&call][unit.Name] = value;
    }
    if (kind == "return") {
      ++Summary.CallReturnValues;
    } else {
      ++Summary.CallClobberValues;
    }
    return value;
  }

  llvm::FunctionCallee callValueHelper(const RegisterUnit &unit,
                                       llvm::StringRef kind) {
    llvm::Module *module = Function.getParent();
    llvm::Type *valueType = unit.Global->getValueType();
    llvm::FunctionType *functionType =
        llvm::FunctionType::get(valueType, {}, false);
    return module->getOrInsertFunction("notdec.register.summary_" + kind.str() +
                                           "." + typeSuffix(*valueType),
                                       functionType);
  }

  llvm::MDNode *registerNode(const RegisterUnit &unit) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(), "name=" + unit.Name),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  llvm::MDNode *callValueNode(const RegisterUnit &unit, llvm::StringRef kind,
                              llvm::Instruction *call) const {
    uint64_t index = 0;
    for (const llvm::Instruction &inst : *call->getParent()) {
      if (&inst == call) {
        break;
      }
      ++index;
    }
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(), "name=" + unit.Name),
        llvm::MDString::get(Function.getContext(), "kind=" + kind.str()),
        llvm::MDString::get(Function.getContext(),
                            "call_index=" + std::to_string(index)),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  void collectFunctionReturnValues() {
    auto shapeIt = SignatureState.Shapes.find(&Function);
    if (shapeIt == SignatureState.Shapes.end() ||
        shapeIt->second.Returns.empty()) {
      return;
    }
    for (llvm::BasicBlock &block : Function) {
      auto *ret =
          llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
      if (ret == nullptr) {
        continue;
      }
      std::vector<llvm::Value *> values;
      values.reserve(shapeIt->second.Returns.size());
      for (const RegisterUnit *unit : shapeIt->second.Returns) {
        llvm::Value *value = resolve(readValueBefore(block, *unit, ret));
        if (value == nullptr ||
            value->getType() != unit->Global->getValueType()) {
          value = frozenPoisonBefore(*ret, unit->Global->getValueType(),
                                     unit->Name + ".return_unknown");
        }
        values.push_back(value);
      }
      SignatureState.FunctionReturns[&Function][ret] = std::move(values);
    }
  }

  llvm::MDNode *markerNode(llvm::StringRef value) const {
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(), value),
    };
    return llvm::MDNode::get(Function.getContext(), fields);
  }

  void attachMetadata() {
    uint64_t phisRemaining = 0;
    for (const auto &[key, phi] : PendingPhi) {
      if (DeadPhis.count(phi) == 0 && phi->getParent() != nullptr) {
        ++phisRemaining;
      }
    }
    llvm::Metadata *fields[] = {
        llvm::MDString::get(Function.getContext(),
                            "loads_replaced=" +
                                std::to_string(Summary.LoadsReplaced)),
        llvm::MDString::get(Function.getContext(),
                            "phis_remaining=" + std::to_string(phisRemaining)),
    };
    Function.setMetadata("notdec.register.summary_ssa",
                         llvm::MDNode::get(Function.getContext(), fields));
  }
};

void addFunctionSummary(NativeRegisterSummarySSASummary &total,
                        const NativeRegisterSummarySSAFunctionSummary &fn) {
  total.LoadsSeen += fn.LoadsSeen;
  total.StoresSeen += fn.StoresSeen;
  total.LoadsReplaced += fn.LoadsReplaced;
  total.DeadLoadsRemoved += fn.DeadLoadsRemoved;
  total.DeadStoresRemoved += fn.DeadStoresRemoved;
  total.PhisCreated += fn.PhisCreated;
  total.PhisSimplified += fn.PhisSimplified;
  total.EntryInputs += fn.EntryInputs;
  total.CallReturnValues += fn.CallReturnValues;
  total.CallClobberValues += fn.CallClobberValues;
  total.CallArgStoresMarked += fn.CallArgStoresMarked;
  total.CallsRewritten += fn.CallsRewritten;
  total.FunctionsRewritten += fn.FunctionsRewritten;
  total.PreservedCalls += fn.PreservedCalls;
  total.UnknownCallEffects += fn.UnknownCallEffects;
  total.StackFrameAccessesRewritten += fn.StackFrameAccessesRewritten;
  total.StackFramePointerLoadsReplaced += fn.StackFramePointerLoadsReplaced;
  total.StackFrameRegisterLoadsRemoved += fn.StackFrameRegisterLoadsRemoved;
  total.StackFrameRegisterStoresRemoved += fn.StackFrameRegisterStoresRemoved;
  total.StackFrameAllocaLoadsRemoved += fn.StackFrameAllocaLoadsRemoved;
  total.StackFrameAllocaStoresRemoved += fn.StackFrameAllocaStoresRemoved;
  total.StackFrameAllocasRemoved += fn.StackFrameAllocasRemoved;
}

llvm::AttributeList attributesForNewFunction(llvm::Function &oldFunction,
                                             llvm::FunctionType &newType) {
  std::vector<llvm::AttributeSet> argAttrs(newType.getNumParams());
  return llvm::AttributeList::get(
      oldFunction.getContext(), oldFunction.getAttributes().getFnAttrs(),
      oldFunction.getAttributes().getRetAttrs(), argAttrs);
}

void copyFunctionMetadata(llvm::Function &from, llvm::Function &to) {
  llvm::SmallVector<std::pair<unsigned, llvm::MDNode *>, 4> metadata;
  from.getAllMetadata(metadata);
  for (const auto &[kind, node] : metadata) {
    to.addMetadata(kind, *node);
  }
}

llvm::Function *createReplacementFunction(llvm::Function &oldFunction,
                                          llvm::FunctionType &newType) {
  llvm::Function *newFunction = llvm::Function::Create(
      &newType, oldFunction.getLinkage(), oldFunction.getAddressSpace());
  newFunction->copyAttributesFrom(&oldFunction);
  newFunction->setAttributes(attributesForNewFunction(oldFunction, newType));
  copyFunctionMetadata(oldFunction, *newFunction);
  newFunction->setComdat(oldFunction.getComdat());
  newFunction->setCallingConv(oldFunction.getCallingConv());
  oldFunction.getParent()->getFunctionList().insert(oldFunction.getIterator(),
                                                    newFunction);
  newFunction->takeName(&oldFunction);
  return newFunction;
}

llvm::Value *buildReturnValue(llvm::IRBuilder<> &builder,
                              const SignatureShape &shape,
                              const std::vector<llvm::Value *> &values) {
  if (shape.Returns.empty()) {
    return nullptr;
  }
  if (shape.Returns.size() == 1) {
    llvm::Type *retTy = singleReturnType(shape);
    if (values.empty() || values.front()->getType() != retTy) {
      return frozenPoisonAt(builder, retTy, "notdec.return_unknown");
    }
    return values.front();
  }
  llvm::Type *retTy = returnTypeForShape(builder.getContext(), shape);
  llvm::Value *result = llvm::UndefValue::get(retTy);
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    llvm::Value *value =
        index < values.size()
            ? values[index]
            : frozenPoisonAt(builder,
                             shape.Returns[index]->Global->getValueType(),
                             shape.Returns[index]->Name + ".return_unknown");
    if (value->getType() != shape.Returns[index]->Global->getValueType()) {
      value = frozenPoisonAt(builder,
                             shape.Returns[index]->Global->getValueType(),
                             shape.Returns[index]->Name + ".return_unknown");
    }
    result = builder.CreateInsertValue(result, value, {index});
  }
  return result;
}

llvm::Value *extractReturnRegister(llvm::IRBuilder<> &builder,
                                   const SignatureShape &shape,
                                   llvm::Value &call,
                                   llvm::StringRef registerName) {
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    if (shape.Returns[index]->Name != registerName) {
      continue;
    }
    if (shape.Returns.size() == 1) {
      return &call;
    }
    return builder.CreateExtractValue(&call, {index},
                                      registerName.str() + ".ret");
  }
  return nullptr;
}

llvm::Value *foreignArgumentReplacement(llvm::Function &function,
                                        llvm::Argument &argument,
                                        std::map<llvm::Type *, llvm::Value *>
                                            &unknownByType) {
  for (llvm::Argument &candidate : function.args()) {
    if (candidate.getName() == argument.getName() &&
        candidate.getType() == argument.getType()) {
      return &candidate;
    }
  }
  auto cached = unknownByType.find(argument.getType());
  if (cached != unknownByType.end()) {
    return cached->second;
  }
  llvm::IRBuilder<> builder(&function.getEntryBlock(),
                            function.getEntryBlock().getFirstNonPHIIt());
  llvm::Value *unknown =
      frozenPoisonAt(builder, argument.getType(), argument.getName() + ".old");
  unknownByType.emplace(argument.getType(), unknown);
  return unknown;
}

void replaceForeignArgumentsInBody(llvm::Function &function) {
  std::map<llvm::Type *, llvm::Value *> unknownByType;
  for (llvm::Instruction &inst : llvm::instructions(function)) {
    for (llvm::Use &operand : inst.operands()) {
      auto *argument = llvm::dyn_cast<llvm::Argument>(operand.get());
      if (argument == nullptr || argument->getParent() == &function) {
        continue;
      }
      operand.set(
          foreignArgumentReplacement(function, *argument, unknownByType));
    }
  }
}

llvm::Value *localizeReturnValue(llvm::Function &function,
                                 llvm::ReturnInst &insertBefore,
                                 llvm::Value *value) {
  auto *argument = llvm::dyn_cast<llvm::Argument>(value);
  if (argument != nullptr && argument->getParent() != &function) {
    std::map<llvm::Type *, llvm::Value *> unknownByType;
    return foreignArgumentReplacement(function, *argument, unknownByType);
  }
  auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction != nullptr && instruction->getFunction() != &function) {
    return frozenPoisonBefore(insertBefore, value->getType(),
                              value->getName() + ".old");
  }
  return value;
}

llvm::Value *localizeCallArgument(llvm::Function &function,
                                  llvm::Instruction &insertBefore,
                                  llvm::Value *value) {
  auto *argument = llvm::dyn_cast<llvm::Argument>(value);
  if (argument != nullptr && argument->getParent() != &function) {
    std::map<llvm::Type *, llvm::Value *> unknownByType;
    return foreignArgumentReplacement(function, *argument, unknownByType);
  }
  auto *instruction = llvm::dyn_cast<llvm::Instruction>(value);
  if (instruction != nullptr && instruction->getFunction() != &function) {
    return frozenPoisonBefore(insertBefore, value->getType(),
                              value->getName() + ".old");
  }
  return value;
}

llvm::CallInst *rewriteCallInst(llvm::CallBase &oldCall, llvm::Value &callee,
                                llvm::FunctionType &newType,
                                const std::vector<llvm::Value *> &args) {
  llvm::SmallVector<llvm::OperandBundleDef, 2> bundles;
  oldCall.getOperandBundlesAsDefs(bundles);
  llvm::CallInst *newCall = llvm::CallInst::Create(
      &newType, &callee, args, bundles, "", oldCall.getIterator());
  if (auto *oldCallInst = llvm::dyn_cast<llvm::CallInst>(&oldCall)) {
    newCall->setTailCallKind(oldCallInst->getTailCallKind());
  }
  newCall->setCallingConv(oldCall.getCallingConv());
  std::vector<llvm::AttributeSet> argAttrs(args.size());
  newCall->setAttributes(llvm::AttributeList::get(
      oldCall.getContext(), oldCall.getAttributes().getFnAttrs(),
      oldCall.getAttributes().getRetAttrs(), argAttrs));
  newCall->copyMetadata(oldCall);
  return newCall;
}

llvm::Value *replacementForOldCallUses(llvm::IRBuilder<> &builder,
                                       llvm::CallBase &oldCall,
                                       llvm::CallInst &newCall,
                                       const SignatureShape &shape) {
  if (oldCall.getType()->isVoidTy()) {
    return nullptr;
  }
  if (oldCall.getType() == newCall.getType()) {
    return &newCall;
  }
  for (unsigned index = 0; index < shape.Returns.size(); ++index) {
    if (shape.Returns[index]->Global->getValueType() != oldCall.getType()) {
      continue;
    }
    if (shape.Returns.size() == 1) {
      return &newCall;
    }
    return builder.CreateExtractValue(&newCall, {index},
                                      oldCall.getName() + ".ret");
  }
  return nullptr;
}

void rewriteInternalFunctionBody(llvm::Function &oldFunction,
                                 llvm::Function &newFunction,
                                 const SignatureShape &shape,
                                 SignatureRewriteState &state) {
  newFunction.splice(newFunction.end(), &oldFunction);
  unsigned index = 0;
  for (llvm::Argument &arg : newFunction.args()) {
    if (index >= shape.Params.size()) {
      break;
    }
    arg.setName(shape.Params[index]->Name + ".arg");
    for (llvm::BasicBlock &block : newFunction) {
      for (auto it = block.begin(); it != block.end();) {
        llvm::Instruction &inst = *it++;
        auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst);
        if (load == nullptr ||
            load->getMetadata("notdec.register.summary_ssa.entry") == nullptr) {
          continue;
        }
        auto *global = llvm::dyn_cast<llvm::GlobalVariable>(
            load->getPointerOperand()->stripPointerCasts());
        if (global == shape.Params[index]->Global) {
          state.ValueMap[load] = &arg;
          load->replaceAllUsesWith(&arg);
        }
      }
    }
    ++index;
  }
  replaceForeignArgumentsInBody(newFunction);

  auto returnsIt = state.FunctionReturns.find(&oldFunction);
  for (llvm::BasicBlock &block : newFunction) {
    auto *oldRet =
        llvm::dyn_cast_or_null<llvm::ReturnInst>(block.getTerminator());
    if (oldRet == nullptr) {
      continue;
    }
    std::vector<llvm::Value *> values;
    if (returnsIt != state.FunctionReturns.end()) {
      auto valueIt = returnsIt->second.find(oldRet);
      if (valueIt != returnsIt->second.end()) {
        values = valueIt->second;
      }
    }
    llvm::IRBuilder<> builder(oldRet);
    for (llvm::Value *&value : values) {
      while (state.ValueMap.count(value) != 0 &&
             state.ValueMap[value] != value) {
        value = state.ValueMap[value];
      }
      value = localizeReturnValue(newFunction, *oldRet, value);
    }
    if (shape.Returns.empty()) {
      builder.CreateRetVoid();
    } else {
      builder.CreateRet(buildReturnValue(builder, shape, values));
    }
    oldRet->eraseFromParent();
  }
}

void rewriteSignatureShapes(llvm::Module &module, SignatureRewriteState &state,
                            NativeRegisterSummarySSASummary &summary) {
  std::map<llvm::Function *, llvm::Function *> replacements;
  std::vector<std::pair<llvm::Function *, SignatureShape>> replacementShapes;
  for (auto &[function, shape] : state.Shapes) {
    llvm::FunctionType *newType =
        functionTypeForShape(module.getContext(), shape);
    if (function->getFunctionType() == newType) {
      replacements[function] = function;
      continue;
    }
    llvm::Function *newFunction =
        createReplacementFunction(*function, *newType);
    replacements[function] = newFunction;
    replacementShapes.emplace_back(newFunction, shape);
    if (!function->isDeclaration()) {
      rewriteInternalFunctionBody(*function, *newFunction, shape, state);
    }
    ++summary.FunctionsRewritten;
  }

  std::vector<llvm::CallBase *> callsToRewrite;
  for (llvm::Function &function : module) {
    for (llvm::Instruction &inst : llvm::instructions(function)) {
      auto *call = llvm::dyn_cast<llvm::CallBase>(&inst);
      if (call != nullptr && state.CallArgs.count(call) != 0) {
        callsToRewrite.push_back(call);
      }
    }
  }

  std::map<llvm::Value *, llvm::Value *> valueMap;
  valueMap.insert(state.ValueMap.begin(), state.ValueMap.end());
  std::vector<llvm::CallBase *> oldCallsToErase;
  std::vector<llvm::CallInst *> helpersToErase;
  auto remapValue = [&](llvm::Value *value) -> llvm::Value * {
    while (valueMap.count(value) != 0 && valueMap[value] != value) {
      value = valueMap[value];
    }
    return value;
  };

  for (llvm::CallBase *oldCall : callsToRewrite) {
    if (oldCall->getParent() == nullptr) {
      continue;
    }
    std::vector<CallArgStoreBinding> &bindings = state.CallArgs[oldCall];
    llvm::Function *oldCallee = oldCall->getCalledFunction();
    if (oldCallee == nullptr || state.Shapes.count(oldCallee) == 0) {
      continue;
    }
    const SignatureShape &shape = state.Shapes.at(oldCallee);
    llvm::Function *newCallee = replacements[oldCallee];
    if (newCallee == nullptr) {
      continue;
    }
    std::vector<llvm::Value *> args;
    args.reserve(bindings.size());
    llvm::IRBuilder<> oldCallBuilder(oldCall);
    for (unsigned index = 0; index < shape.Params.size(); ++index) {
      llvm::Value *value =
          index < bindings.size() ? bindings[index].Value : nullptr;
      if (value != nullptr) {
        value = remapValue(value);
        value = localizeCallArgument(*oldCall->getFunction(), *oldCall, value);
      }
      if (value == nullptr ||
          value->getType() != shape.Params[index]->Global->getValueType()) {
        value = frozenPoisonAt(
            oldCallBuilder, shape.Params[index]->Global->getValueType(),
            shape.Params[index]->Name + ".arg_unknown");
      }
      args.push_back(value);
    }
    if (shape.VarArg) {
      for (const CallArgStoreBinding &binding : bindings) {
        if (binding.Index < shape.Params.size()) {
          continue;
        }
        llvm::Value *value = binding.Value;
        if (value == nullptr) {
          continue;
        }
        value = remapValue(value);
        value = localizeCallArgument(*oldCall->getFunction(), *oldCall, value);
        args.push_back(value);
      }
    }
    llvm::CallInst *newCall = rewriteCallInst(
        *oldCall, *newCallee, *newCallee->getFunctionType(), args);
    state.RewrittenCalls.insert(newCall);
    if (!oldCall->use_empty()) {
      llvm::IRBuilder<> builder(newCall->getNextNode());
      llvm::Value *replacement =
          replacementForOldCallUses(builder, *oldCall, *newCall, shape);
      if (replacement != nullptr) {
        oldCall->replaceAllUsesWith(replacement);
        newCall->takeName(oldCall);
      }
    }
    valueMap[oldCall] = newCall;
    oldCallsToErase.push_back(oldCall);
    auto helpersIt = state.ReturnHelpers.find(oldCall);
    if (helpersIt != state.ReturnHelpers.end() && !shape.Returns.empty()) {
      llvm::IRBuilder<> builder(newCall->getNextNode());
      for (auto &[name, helper] : helpersIt->second) {
        if (helper->getParent() == nullptr) {
          continue;
        }
        llvm::Value *value =
            extractReturnRegister(builder, shape, *newCall, name);
        if (value == nullptr) {
          value = frozenPoisonAt(builder, helper->getType(),
                                 name + ".return_unknown");
        }
        valueMap[helper] = value;
        helper->replaceAllUsesWith(value);
        if (helper->use_empty()) {
          helpersToErase.push_back(helper);
        }
      }
    }
    ++summary.CallsRewritten;
  }

  for (llvm::CallInst *helper : helpersToErase) {
    if (helper->getParent() != nullptr && helper->use_empty()) {
      helper->eraseFromParent();
    }
  }
  for (llvm::CallBase *call : oldCallsToErase) {
    if (call->getParent() != nullptr && call->use_empty()) {
      call->eraseFromParent();
    }
  }

  for (llvm::StoreInst *store : state.StoresToErase) {
    if (store->getParent() != nullptr && store->use_empty()) {
      store->eraseFromParent();
      ++summary.DeadStoresRemoved;
    }
  }

  for (auto &[oldFunction, newFunction] : replacements) {
    if (oldFunction != newFunction && oldFunction->use_empty() &&
        oldFunction->empty()) {
      oldFunction->eraseFromParent();
    }
  }
  for (auto &[function, shape] : replacementShapes) {
    state.Shapes[function] = std::move(shape);
  }
}

} // namespace

NativeRegisterSummarySSASummary
runNativeRegisterSummarySSA(llvm::Module &module,
                            const NativeRegisterSummarySSAOptions &options) {
  truncateKnownNoReturnExternalCalls(module);

  NativeRegisterSummaryOptions summaryOptions;
  summaryOptions.AttachMetadata = true;
  NativeStackFrameRewriteSummary stackFrameSummary =
      runNativeStackFrameRewrite(module);
  NativeRegisterSummarySSAOptions effectiveOptions = options;
  effectiveOptions.IgnoredRegisters.insert(
      stackFrameSummary.IgnoredRegisters.begin(),
      stackFrameSummary.IgnoredRegisters.end());
  summaryOptions.IgnoredRegisters = effectiveOptions.IgnoredRegisters;
  NativeRegisterSummary registerSummary =
      runNativeRegisterSummary(module, summaryOptions);
  std::map<llvm::Function *, FunctionSummaryFacts> facts =
      summaryFactsByFunction(registerSummary, module);
  std::map<llvm::GlobalVariable *, RegisterUnit> units =
      collectRegisterUnits(module);
  AbiFacts abi = collectAbiFacts(module, units);
  SignatureRewriteState signatureState;
  if (options.EnableRewrite) {
    signatureState.Shapes =
        buildInitialSignatureShapes(module, units, facts, abi);
  }

  NativeRegisterSummarySSASummary summary;
  for (llvm::Function &function : module) {
    if (function.isDeclaration()) {
      continue;
    }
    NativeRegisterSummarySSAFunctionSummary fn;
    FunctionBuilder builder(function, units, facts, abi, options, fn,
                            signatureState);
    builder.run();
    addFunctionSummary(summary, fn);
    summary.Functions.push_back(std::move(fn));
  }
  if (options.EnableRewrite) {
    addDemandedExternalReturns(signatureState, units, abi);
    rewriteSignatureShapes(module, signatureState, summary);
    eraseUnusedSummaryHelperDeclarations(module);
    if (options.EnableResidueRemoval) {
      for (llvm::Function &function : module) {
        if (function.isDeclaration()) {
          continue;
        }
        NativeRegisterSummarySSAFunctionSummary cleanupFn;
        FunctionBuilder cleanup(function, units, facts, abi, options, cleanupFn,
                                signatureState);
        cleanup.removeDeadStoresAfterSignatureRewrite();
        summary.DeadStoresRemoved += cleanupFn.DeadStoresRemoved;
      }
      NativeStackFrameCleanupOptions cleanupOptions;
      cleanupOptions.StackPointerRegister = stackFrameSummary.StackPointerRegister;
      cleanupOptions.Registers = effectiveOptions.IgnoredRegisters;
      NativeStackFrameCleanupSummary cleanupSummary =
          runNativeStackFrameCleanup(module, cleanupOptions);
      summary.StackFrameAccessesRewritten += cleanupSummary.AccessesRewritten;
      summary.StackFramePointerLoadsReplaced +=
          cleanupSummary.FramePointerLoadsReplaced;
      summary.StackFrameRegisterLoadsRemoved +=
          cleanupSummary.RegisterLoadsRemoved;
      summary.StackFrameRegisterStoresRemoved +=
          cleanupSummary.RegisterStoresRemoved;
      summary.StackFrameAllocaLoadsRemoved +=
          cleanupSummary.StackAllocaLoadsRemoved;
      summary.StackFrameAllocaStoresRemoved +=
          cleanupSummary.StackAllocaStoresRemoved;
      summary.StackFrameAllocasRemoved += cleanupSummary.StackAllocasRemoved;
    }
  }
  summary.FunctionsSeen = summary.Functions.size();
  if (options.PrintSummary) {
    printNativeRegisterSummarySSASummary(summary, llvm::errs());
  }
  return summary;
}

void printNativeRegisterSummarySSASummary(
    const NativeRegisterSummarySSASummary &summary, llvm::raw_ostream &os) {
  os << "Native register summary SSA: functions=" << summary.FunctionsSeen
     << " loads=" << summary.LoadsSeen << " stores=" << summary.StoresSeen
     << " loads_replaced=" << summary.LoadsReplaced
     << " dead_loads_removed=" << summary.DeadLoadsRemoved
     << " dead_stores_removed=" << summary.DeadStoresRemoved
     << " phis_created=" << summary.PhisCreated
     << " phis_simplified=" << summary.PhisSimplified
     << " entry_inputs=" << summary.EntryInputs
     << " call_returns=" << summary.CallReturnValues
     << " call_clobbers=" << summary.CallClobberValues
     << " call_arg_stores_marked=" << summary.CallArgStoresMarked
     << " preserved_calls=" << summary.PreservedCalls
     << " unknown_call_effects=" << summary.UnknownCallEffects
     << " stack_frame_accesses_rewritten="
     << summary.StackFrameAccessesRewritten
     << " stack_frame_pointer_loads_replaced="
     << summary.StackFramePointerLoadsReplaced
     << " stack_frame_register_loads_removed="
     << summary.StackFrameRegisterLoadsRemoved
     << " stack_frame_register_stores_removed="
     << summary.StackFrameRegisterStoresRemoved
     << " stack_frame_alloca_loads_removed="
     << summary.StackFrameAllocaLoadsRemoved
     << " stack_frame_alloca_stores_removed="
     << summary.StackFrameAllocaStoresRemoved
     << " stack_frame_allocas_removed=" << summary.StackFrameAllocasRemoved
     << "\n";
  for (const NativeRegisterSummarySSAFunctionSummary &function :
       summary.Functions) {
    os << "  " << function.FunctionName << ": loads=" << function.LoadsSeen
       << " stores=" << function.StoresSeen
       << " loads_replaced=" << function.LoadsReplaced
       << " dead_loads_removed=" << function.DeadLoadsRemoved
       << " dead_stores_removed=" << function.DeadStoresRemoved
       << " phis_created=" << function.PhisCreated
       << " phis_simplified=" << function.PhisSimplified
       << " entry_inputs=" << function.EntryInputs
       << " call_returns=" << function.CallReturnValues
       << " call_clobbers=" << function.CallClobberValues
       << " call_arg_stores_marked=" << function.CallArgStoresMarked
       << " preserved_calls=" << function.PreservedCalls
       << " unknown_call_effects=" << function.UnknownCallEffects << "\n";
  }
}

} // namespace notdec::bin2llvm
