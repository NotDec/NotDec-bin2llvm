#include "notdec-bin2llvm/NativeExternalPrototype.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <optional>
#include <string>

namespace notdec::bin2llvm {
namespace {

using Prototype = NativeExternalPrototype;
using ValueType = NativeExternalPrototype::ValueType;

bool parseValueType(llvm::StringRef text, ValueType &type) {
  if (text == "i32" || text == "int" || text == "pid_t") {
    type = ValueType::I32;
    return true;
  }
  if (text == "i64") {
    type = ValueType::I64;
    return true;
  }
  if (text == "ptr" || text == "pointer" || text == "size_t" ||
      text == "ssize_t" || text == "long" || text == "ulong") {
    type = ValueType::PointerSized;
    return true;
  }
  if (text == "float") {
    type = ValueType::Float;
    return true;
  }
  if (text == "double") {
    type = ValueType::Double;
    return true;
  }
  return false;
}

bool readUnsigned(const llvm::json::Object &object, llvm::StringRef key,
                  unsigned &value, std::string &errorMessage) {
  if (std::optional<int64_t> number = object.getInteger(key)) {
    if (*number < 0) {
      errorMessage = (key + " must be non-negative").str();
      return false;
    }
    value = static_cast<unsigned>(*number);
  }
  return true;
}

bool readBool(const llvm::json::Object &object, llvm::StringRef key,
              bool &value, std::string &errorMessage) {
  if (std::optional<bool> boolean = object.getBoolean(key)) {
    value = *boolean;
    return true;
  }
  if (object.get(key) != nullptr) {
    errorMessage = (key + " must be boolean").str();
    return false;
  }
  return true;
}

bool readTypeArray(const llvm::json::Object &object, llvm::StringRef key,
                   std::vector<ValueType> &types,
                   std::string &errorMessage) {
  const llvm::json::Array *array = object.getArray(key);
  if (array == nullptr) {
    return true;
  }
  for (const llvm::json::Value &entry : *array) {
    std::optional<llvm::StringRef> text = entry.getAsString();
    if (!text) {
      errorMessage = (key + " entries must be strings").str();
      return false;
    }
    ValueType type;
    if (!parseValueType(*text, type)) {
      errorMessage = ("unknown type in " + key + ": " + *text).str();
      return false;
    }
    types.push_back(type);
  }
  return true;
}

std::optional<Prototype> parsePrototype(const llvm::json::Object &object,
                                        std::string &errorMessage) {
  Prototype prototype;
  if (!readUnsigned(object, "fixed_args", prototype.FixedArgs, errorMessage) ||
      !readBool(object, "vararg", prototype.VarArg, errorMessage) ||
      !readBool(object, "noreturn", prototype.NoReturn, errorMessage) ||
      !readUnsigned(object, "max_return_registers",
                    prototype.MaxReturnRegisters, errorMessage) ||
      !readUnsigned(object, "max_args", prototype.MaxArgs, errorMessage) ||
      !readTypeArray(object, "params", prototype.TypedParams, errorMessage)) {
    return std::nullopt;
  }

  if (std::optional<llvm::StringRef> returnText = object.getString("return")) {
    if (*returnText != "void") {
      ValueType type;
      if (!parseValueType(*returnText, type)) {
        errorMessage = ("unknown return type: " + *returnText).str();
        return std::nullopt;
      }
      prototype.TypedReturn = type;
      prototype.MaxReturnRegisters = prototype.NoReturn ? 0 : 1;
    } else {
      prototype.TypedReturn.reset();
      prototype.MaxReturnRegisters = 0;
    }
  }

  if (!prototype.TypedParams.empty()) {
    prototype.FixedArgs = static_cast<unsigned>(prototype.TypedParams.size());
  }
  if (prototype.MaxArgs != 0 && prototype.MaxArgs < prototype.FixedArgs) {
    errorMessage = "max_args must be >= fixed_args";
    return std::nullopt;
  }
  if (prototype.NoReturn) {
    prototype.MaxReturnRegisters = 0;
  }
  return prototype;
}

bool loadPrototypeObject(const llvm::json::Object &object,
                         NativeExternalPrototypeMap &prototypes,
                         std::string &errorMessage) {
  std::optional<llvm::StringRef> name = object.getString("name");
  if (!name || name->empty()) {
    errorMessage = "prototype entry missing non-empty name";
    return false;
  }
  std::optional<Prototype> prototype = parsePrototype(object, errorMessage);
  if (!prototype) {
    errorMessage = name->str() + ": " + errorMessage;
    return false;
  }
  prototypes[name->str()] = std::move(*prototype);
  return true;
}

bool loadPrototypeMap(const llvm::json::Object &object,
                      NativeExternalPrototypeMap &prototypes,
                      std::string &errorMessage) {
  for (const auto &entry : object) {
    const llvm::json::Object *prototypeObject = entry.second.getAsObject();
    if (prototypeObject == nullptr) {
      errorMessage = "prototype map value must be object: " + entry.first.str();
      return false;
    }
    std::optional<Prototype> prototype =
        parsePrototype(*prototypeObject, errorMessage);
    if (!prototype) {
      errorMessage = entry.first.str() + ": " + errorMessage;
      return false;
    }
    prototypes[entry.first.str()] = std::move(*prototype);
  }
  return true;
}

bool loadPrototypeArray(const llvm::json::Array &array,
                        NativeExternalPrototypeMap &prototypes,
                        std::string &errorMessage) {
  for (const llvm::json::Value &entry : array) {
    const llvm::json::Object *object = entry.getAsObject();
    if (object == nullptr) {
      errorMessage = "prototype array entries must be objects";
      return false;
    }
    if (!loadPrototypeObject(*object, prototypes, errorMessage)) {
      return false;
    }
  }
  return true;
}

} // namespace

const NativeExternalPrototypeMap &defaultNativeExternalPrototypes() {
  static const NativeExternalPrototypeMap prototypes = {
      {"__assert_fail", {4, false, true}},
      {"__ctype_b_loc", {0, false, false, 1, {}, ValueType::PointerSized}},
      {"__ctype_tolower_loc",
       {0, false, false, 1, {}, ValueType::PointerSized}},
      {"__ctype_toupper_loc",
       {0, false, false, 1, {}, ValueType::PointerSized}},
      {"__cxa_atexit", {3, false, false}},
      {"__cxa_finalize", {1, false, false}},
      {"__errno_location",
       {0, false, false, 1, {}, ValueType::PointerSized}},
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
      {"__asprintf_chk", {3, true, false}},
      {"__poll_chk", {4, false, false}},
      {"__read_chk", {4, false, false}},
      {"__register_atfork", {4, false, false}},
      {"__sched_cpucount", {2, false, false}},
      {"__snprintf_chk", {5, true, false}},
      {"__sprintf_chk", {4, true, false}},
      {"__strcat_chk", {3, false, false}},
      {"__strcpy_chk", {3, false, false}},
      {"__strncpy_chk", {4, false, false}},
      {"__sigsetjmp", {2, false, false}},
      {"__syslog_chk", {2, true, false}},
      {"__sysconf", {1, false, false}},
      {"__stack_chk_fail", {0, false, true}},
      {"__tls_get_addr", {1, false, false}},
      {"__vasprintf_chk", {4, false, false}},
      {"__vfprintf_chk", {4, false, false}},
      {"__vsnprintf_chk", {6, false, false}},
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
      {"BN_sub", {3, false}},
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
      {"cos", {0, false, false, 1, {ValueType::Double}, ValueType::Double}},
      {"EC_GROUP_get_order", {3, false}},
      {"EC_KEY_set_private_key", {2, false}},
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
      {"exp", {0, false, false, 1, {ValueType::Double}, ValueType::Double}},
      {"fclose", {1, false}},
      {"fcntl", {2, true, false, 1, {}, std::nullopt, 3}},
      {"fcntl64", {2, true, false, 1, {}, std::nullopt, 3}},
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
      {"fork", {0, false, false, 1, {}, ValueType::I32}},
      {"fsync", {1, false}},
      {"ftell", {1, false}},
      {"futimens", {2, false}},
      {"fwrite", {4, false}},
      {"gai_strerror", {1, false}},
      {"getaddrinfo", {4, false}},
      {"getdelim", {4, false}},
      {"getnameinfo", {7, false}},
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
      {"getservbyname", {2, false}},
      {"gethostbyname", {1, false}},
      {"getloadavg", {2, false}},
      {"getpagesize", {0, false}},
      {"getpeername", {3, false}},
      {"getpid", {0, false, false, 1, {}, ValueType::I64}},
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
      {"ioctl", {2, true, false, 1, {}, std::nullopt, 3}},
      {"isatty", {1, false}},
      {"kill", {2, false}},
      {"lchown", {3, false}},
      {"link", {2, false}},
      {"listen", {2, false}},
      {"localtime", {1, false}},
      {"localtime_r", {2, false}},
      {"log", {0, false, false, 1, {ValueType::Double}, ValueType::Double}},
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
      {"mmap", {6, false}},
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
      {"open", {2, true, false, 1, {}, std::nullopt, 3}},
      {"open64", {2, true, false, 1, {}, std::nullopt, 3}},
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
      {"pow",
       {0,
        false,
        false,
        1,
        {ValueType::Double, ValueType::Double},
        ValueType::Double}},
      {"printf", {1, true}},
      {"prctl", {1, true, false, 1, {}, std::nullopt, 3}},
      {"popen", {2, false}},
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
      {"random", {0, false, false, 1, {}, ValueType::I64}},
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
      {"recvfrom", {6, false}},
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
      {"sasl_server_start", {6, false}},
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
      {"SSL_CTX_ctrl", {4, false}},
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
      {"RSA_set0_key", {4, false}},
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
      {"setvbuf", {4, false}},
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
      {"__sysv_signal", {2, false}},
      {"signal", {2, false}},
      {"sigprocmask", {3, false}},
      {"sin", {0, false, false, 1, {ValueType::Double}, ValueType::Double}},
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
      {"splice", {6, false}},
      {"srandom", {1, false}},
      {"srand", {1, false}},
      {"sqrt", {0, false, false, 1, {ValueType::Double}, ValueType::Double}},
      {"sscanf", {2, true}},
      {"stat", {2, false}},
      {"stat64", {2, false}},
      {"statfs64", {2, false}},
      {"strcasecmp", {2, false}},
      {"strcat", {2, false}},
      {"strchr", {2, false}},
      {"strchrnul", {2, false}},
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
      {"getline", {3, false}},
      {"strtok", {2, false}},
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

const NativeExternalPrototype *
lookupNativeExternalPrototype(const NativeExternalPrototypeMap &prototypes,
                              llvm::StringRef name) {
  auto it = prototypes.find(name);
  return it == prototypes.end() ? nullptr : &it->second;
}

std::optional<NativeExternalPrototypeMap>
loadNativeExternalPrototypesJson(llvm::StringRef path,
                                 std::string &errorMessage) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    errorMessage = "failed to read " + path.str() + ": " +
                   buffer.getError().message();
    return std::nullopt;
  }

  auto parsed = llvm::json::parse(buffer.get()->getBuffer());
  if (!parsed) {
    std::string message;
    llvm::raw_string_ostream os(message);
    llvm::logAllUnhandledErrors(parsed.takeError(), os, "");
    errorMessage = "failed to parse " + path.str() + ": " + message;
    return std::nullopt;
  }

  NativeExternalPrototypeMap prototypes = defaultNativeExternalPrototypes();
  if (const llvm::json::Object *root = parsed->getAsObject()) {
    if (const llvm::json::Array *array = root->getArray("prototypes")) {
      if (!loadPrototypeArray(*array, prototypes, errorMessage)) {
        errorMessage = path.str() + ": " + errorMessage;
        return std::nullopt;
      }
      return prototypes;
    }
    if (!loadPrototypeMap(*root, prototypes, errorMessage)) {
      errorMessage = path.str() + ": " + errorMessage;
      return std::nullopt;
    }
    return prototypes;
  }
  if (const llvm::json::Array *array = parsed->getAsArray()) {
    if (!loadPrototypeArray(*array, prototypes, errorMessage)) {
      errorMessage = path.str() + ": " + errorMessage;
      return std::nullopt;
    }
    return prototypes;
  }

  errorMessage = path.str() + ": root must be object or array";
  return std::nullopt;
}

} // namespace notdec::bin2llvm
