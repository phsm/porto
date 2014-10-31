#include "subsystem.hpp"
#include "property.hpp"
#include "cgroup.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/pwd.hpp"

bool TPropertyHolder::ParentDefault(std::shared_ptr<TContainer> &c,
                                    const std::string &property) {
    TError error = GetSharedContainer(c);
    if (error) {
        TLogger::LogError(error, "Can't get default for " + property);
        return "";
    }

    return c->UseParentNamespace() && HasFlags(property, PARENT_DEF_PROPERTY);
}

bool TPropertyHolder::IsDefault(const std::string &property) {
    return Holder.IsDefault(property);
}

std::string TPropertyHolder::Get(const std::string &property) {
    if (Holder.IsDefault(property)) {
        std::shared_ptr<TContainer> c;
        if (ParentDefault(c, property))
            return c->GetParent()->Prop->Get(property);
    }

    std::shared_ptr<TValueState> s;
    TError error = Holder.Get(property, s);
    TLogger::LogError(error, "Can't get property " + property);
    return s->Get();
}

bool TPropertyHolder::GetBool(const std::string &property) {
    if (Holder.IsDefault(property)) {
        std::shared_ptr<TContainer> c;
        if (ParentDefault(c, property))
            return c->GetParent()->Prop->GetBool(property);
    }

    std::shared_ptr<TValueState> s;
    TError error = Holder.Get(property, s);
    TLogger::LogError(error, "Can't get property " + property);
    return s->GetBool();
}

int TPropertyHolder::GetInt(const std::string &property) {
    if (Holder.IsDefault(property)) {
        std::shared_ptr<TContainer> c;
        if (ParentDefault(c, property))
            return c->GetParent()->Prop->GetInt(property);
    }

    int val;

    if (StringToInt(Get(property), val))
        return 0;

    return val;
}

uint64_t TPropertyHolder::GetUint(const std::string &property) {
    if (Holder.IsDefault(property)) {
        std::shared_ptr<TContainer> c;
        if (ParentDefault(c, property))
            return c->GetParent()->Prop->GetUint(property);
    }

    uint64_t val;

    if (StringToUint64(Get(property), val))
        return 0;

    return val;
}

TError TPropertyHolder::GetRaw(const std::string &property, std::string &value) {
    std::shared_ptr<TValueState> s;
    TError error = Holder.Get(property, s);
    if (error)
        return error;
    value = s->Get();
    return TError::Success();
}

// TODO: return TError
void TPropertyHolder::SetRaw(const std::string &property,
                             const std::string &value) {
    std::shared_ptr<TValueState> s;
    TError error = Holder.Get(property, s);
    if (error)
        TLogger::LogError(error, "Can't set raw property " + property);

    s->SetRaw(value);
    error = AppendStorage(property, value);
    if (error)
        TLogger::LogError(error, "Can't append property to key-value store");
}

TError TPropertyHolder::Set(const std::string &property,
                            const std::string &value) {
    if (!propertySpec.Valid(property)) {
        TError error(EError::InvalidValue, "property not found");
        TLogger::LogError(error, "Can't set property");
        return error;
    }

    std::shared_ptr<TValueState> s;
    TError error = Holder.Get(property, s);
    if (error)
        return error;

    error = s->Set(value);
    if (error)
        return error;

    error = AppendStorage(property, value);
    if (error)
        return error;

    return TError::Success();
}

bool TPropertyHolder::HasFlags(const std::string &property, int flags) {
    // TODO: Log error
    if (!propertySpec.Valid(property))
        return false;

    return propertySpec.Get(property)->Flags & flags;
}
bool TPropertyHolder::HasState(const std::string &property, EContainerState state) {
    // TODO: Log error
    if (!propertySpec.Valid(property))
        return false;
    auto p = propertySpec.Get(property);

    return p->State.find(state) != p->State.end();
}

bool TPropertyHolder::IsRoot() {
    return Name == ROOT_CONTAINER;
}

TError TPropertyHolder::Create() {
    kv::TNode node;
    return Storage.SaveNode(Name, node);
}

TError TPropertyHolder::Restore(const kv::TNode &node) {
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();
        auto value = node.pairs(i).val();

        SetRaw(key, value);
    }

    return SyncStorage();
}

TError TPropertyHolder::PropertyExists(const std::string &property) {
    if (!propertySpec.Valid(property))
        return TError(EError::InvalidProperty, "invalid property");
    return TError::Success();
}

TPropertyHolder::~TPropertyHolder() {
    if (!IsRoot()) {
        TError error = Storage.RemoveNode(Name);
        TLogger::LogError(error, "Can't remove key-value node " + Name);
    }
}

TError TPropertyHolder::SyncStorage() {
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    for (auto &kv : Holder.State) {
        auto pair = node.add_pairs();
        pair->set_key(kv.first);
        pair->set_val(kv.second->Get());
    }

    return Storage.SaveNode(Name, node);
}

TError TPropertyHolder::GetSharedContainer(std::shared_ptr<TContainer> &c) {
    c = Container.lock();
    if (!c)
        return TError(EError::Unknown, "Can't convert weak container reference");

    return TError::Success();
}

TError TPropertyHolder::AppendStorage(const std::string& key, const std::string& value) {
    if (IsRoot())
        return TError::Success();

    kv::TNode node;

    auto pair = node.add_pairs();
    pair->set_key(key);
    pair->set_val(value);

    return Storage.AppendNode(Name, node);
}

static TError ValidUint(std::shared_ptr<TContainer> container, const std::string &str) {
    uint32_t val;
    if (StringToUint32(str, val))
        return TError(EError::InvalidValue, "invalid numeric value");

    return TError::Success();
}

static TError ValidPath(std::shared_ptr<TContainer> c, const std::string &str) {
    if (!str.length() || str[0] != '/')
        return TError(EError::InvalidValue, "invalid directory");
    return TError::Success();
}

static TError ExistingFile(std::shared_ptr<TContainer> c, const std::string &str) {
    TFile f(str);
    if (!f.Exists())
        return TError(EError::InvalidValue, "file doesn't exist");
    return TError::Success();
}

static std::string DefaultStdFile(std::shared_ptr<TContainer> c,
                                  const std::string &name) {
    std::string cwd, root;
    TError error = c->GetProperty("cwd", cwd);
    TLogger::LogError(error, "Can't get cwd for std file");
    if (error)
        return "";

    error = c->GetProperty("root", root);
    TLogger::LogError(error, "Can't get root for std file");
    if (error)
        return "";

    std::string prefix;
    if (c->UseParentNamespace())
        prefix = c->GetName(false) + ".";

    TPath path = root;
    path.AddComponent(cwd);
    path.AddComponent(prefix + name);
    return path.ToString();
}

static std::set<EContainerState> staticProperty = {
    EContainerState::Stopped,
};

static std::set<EContainerState> dynamicProperty = {
    EContainerState::Stopped,
    EContainerState::Running,
    EContainerState::Paused,
    EContainerState::Meta,
};

class TCommandProperty : public TStringValue {
public:
    TCommandProperty() :
        TStringValue("command",
                     "Command executed upon container start",
                     0,
                     staticProperty) {}
};

class TUserProperty : public TStringValue {
public:
    TUserProperty() :
        TStringValue("user",
                     "Start command with given user",
                     SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        int uid, gid;
        c->GetPerm(uid, gid);
        TUser u(uid);
        if (u.Load())
            return std::to_string(uid);
        else
            return u.GetName();
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        TUser u(value);
        return u.Load();
    }
};

class TGroupProperty : public TStringValue {
public:
    TGroupProperty() :
        TStringValue("group",
                     "Start command with given group",
                     SUPERUSER_PROPERTY | PARENT_DEF_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        int uid, gid;
        c->GetPerm(uid, gid);
        TGroup g(gid);
        if (g.Load())
            return std::to_string(gid);
        else
            return g.GetName();
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        TGroup g(value);
        return g.Load();
    }
};

class TEnvProperty : public TStringValue { // TODO: EValueType::List,
public:
    TEnvProperty() :
        TStringValue("env",
                     "Container environment variables",
                     PARENT_DEF_PROPERTY,
                     staticProperty) {}
};

class TRootProperty : public TStringValue {
public:
    TRootProperty() :
        TStringValue("root",
                     "Container root directory",
                     PARENT_DEF_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "/";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        return ValidPath(c, value);
    }
};

class TCwdProperty : public TStringValue {
public:
    TCwdProperty() :
        TStringValue("cwd",
                     "Container working directory",
                     PARENT_DEF_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        if (!c->Prop->IsDefault("root"))
            return "/";

        return config().container().tmp_dir() + "/" + c->GetName();
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        return ValidPath(c, value);
    }
};

class TStdinPathProperty : public TStringValue {
public:
    TStdinPathProperty() :
        TStringValue("stdin_path",
                     "Container standard input path",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "/dev/null";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        return ExistingFile(c, value);
    }
};

class TStdoutPathProperty : public TStringValue {
public:
    TStdoutPathProperty() :
        TStringValue("stdout_path",
                     "Container standard input path",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return DefaultStdFile(c, "stdout");
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        return ValidPath(c, value);
    }
};

class TStderrPathProperty : public TStringValue {
public:
    TStderrPathProperty() :
        TStringValue("stderr_path",
                     "Container standard error path",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return DefaultStdFile(c, "stderr");
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        return ValidPath(c, value);
    }
};

class TStdoutLimitProperty : public TStringValue {
public:
    TStdoutLimitProperty() :
        TStringValue("stdout_limit",
                     "Return no more than given number of bytes from standard output/error",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return std::to_string(config().container().stdout_limit());
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        uint32_t num;
        uint32_t max = config().container().stdout_limit();

        TError error = StringToUint32(value, num);
        if (error)
            return error;

        if (num > max)
            return TError(EError::InvalidValue,
                          "Maximum number of bytes: " +
                          std::to_string(max));

        return TError::Success();
    }
};

class TMemoryGuaranteeProperty : public TStringValue {
public:
    TMemoryGuaranteeProperty() :
        TStringValue("memory_guarantee",
                     "Guaranteed amount of memory",
                     PARENT_RO_PROPERTY,
                     dynamicProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "0";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        uint64_t num;

        auto memroot = memorySubsystem->GetRootCgroup();
        if (!memroot->HasKnob("memory.low_limit_in_bytes"))
            return TError(EError::NotSupported, "invalid kernel");

        if (StringToUint64(value, num))
            return TError(EError::InvalidValue, "invalid value");

        if (!c->ValidHierarchicalProperty("memory_guarantee", value))
            return TError(EError::InvalidValue, "invalid hierarchical value");

        uint64_t total = c->GetRoot()->GetChildrenSum("memory_guarantee", c, num);
        if (total + config().daemon().memory_guarantee_reserve() > GetTotalMemory())
            return TError(EError::ResourceNotAvailable, "can't guarantee all available memory");

        return TError::Success();
    }
};

class TMemoryLimitProperty : public TStringValue { // TODO: Uint
public:
    TMemoryLimitProperty() :
        TStringValue("memory_limit",
                     "Memory hard limit",
                     0,
                     dynamicProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "0";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        uint64_t num;

        if (StringToUint64(value, num))
            return TError(EError::InvalidValue, "invalid value");

        if (!c->ValidHierarchicalProperty("memory_limit", value))
            return TError(EError::InvalidValue, "invalid hierarchical value");

        return TError::Success();
    }
};

class TRechargeOnPgfaultProperty : public TBoolValue {
public:
    TRechargeOnPgfaultProperty() :
        TBoolValue("recharge_on_pgfault",
                   "Recharge memory on page fault",
                   PARENT_RO_PROPERTY,
                   dynamicProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) {
        return false;
    }

    TError SetBool(std::shared_ptr<TContainer> c,
                   std::shared_ptr<TValueState> s,
                   const bool value) {
        auto memroot = memorySubsystem->GetRootCgroup();
        if (!memroot->HasKnob("memory.recharge_on_pgfault"))
            return TError(EError::NotSupported, "invalid kernel");

        return TError::Success();
    }
};

class TCpuPolicyProperty : public TStringValue {
public:
    TCpuPolicyProperty() :
        TStringValue("cpu_policy",
                     "CPU policy: rt, normal, idle",
                     PARENT_RO_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "normal";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        if (value != "normal" && value != "rt" && value != "idle")
            return TError(EError::InvalidValue, "invalid policy");

        if (value == "rt") {
            auto cpuroot = cpuSubsystem->GetRootCgroup();
            if (!cpuroot->HasKnob("cpu.smart"))
                return TError(EError::NotSupported, "invalid kernel");
        }

        if (value == "idle")
            return TError(EError::NotSupported, "not implemented");

        return TError::Success();
    }
};

class TCpuPriorityProperty : public TStringValue {
public:
    TCpuPriorityProperty() :
        TStringValue("cpu_priority",
                     "CPU priority: 0-99",
                     PARENT_RO_PROPERTY,
                     dynamicProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return std::to_string(DEF_CLASS_PRIO);
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        int num;

        if (StringToInt(value, num))
            return TError(EError::InvalidValue, "invalid value");

        if (num < 0 || num > 99)
            return TError(EError::InvalidValue, "invalid value");

        return TError::Success();
    }
};

class TNetGuaranteeProperty : public TStringValue {
public:
    TNetGuaranteeProperty() :
        TStringValue("net_guarantee",
                     "Guaranteed container network bandwidth",
                     PARENT_RO_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return std::to_string(DEF_CLASS_RATE);
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        return ValidUint(c, value);
    }
};

class TNetCeilProperty : public TStringValue {
public:
    TNetCeilProperty() :
        TStringValue("net_ceil",
                     "Maximum container network bandwidth",
                     PARENT_RO_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return std::to_string(DEF_CLASS_CEIL);
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        return ValidUint(c, value);
    }
};

class TNetPriorityProperty : public TStringValue {
public:
    TNetPriorityProperty() :
        TStringValue("net_priority",
                     "Container network priority: 0-7",
                     PARENT_RO_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return std::to_string(DEF_CLASS_NET_PRIO);
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        int num;

        if (StringToInt(value, num))
            return TError(EError::InvalidValue, "invalid value");

        if (num < 0 || num > 7)
            return TError(EError::InvalidValue, "invalid value");

        return TError::Success();
    }
};

class TRespawnProperty : public TBoolValue {
public:
    TRespawnProperty() :
        TBoolValue("respawn",
                   "Automatically respawn dead container",
                   0,
                   staticProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) {
        return false;
    }
};

class TMaxRespawnsProperty : public TStringValue {
public:
    TMaxRespawnsProperty() :
        TStringValue("max_respawns",
                     "Limit respawn count for specific container",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "-1";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        return ValidUint(c, value);
    }
};

class TIsolateProperty : public TBoolValue {
public:
    TIsolateProperty() :
        TBoolValue("isolate",
                   "Isolate container from parent",
                   0,
                   staticProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) {
        return true;
    }
};

class TPrivateProperty : public TStringValue {
public:
    TPrivateProperty() :
        TStringValue("private",
                     "User-defined property",
                     0,
                     dynamicProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        uint32_t max = config().container().private_max();

        if (value.length() > max)
            return TError(EError::InvalidValue, "Value is too long");

        return TError::Success();
    }
};

class TUlimitProperty : public TStringValue { //TODO: MAP
public:
    TUlimitProperty() :
        TStringValue("ulimit",
                     "Container resource limits",
                     PARENT_DEF_PROPERTY,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        std::map<int, struct rlimit> rlim;
        return ParseRlimit(value, rlim);
    }
};

class THostnameProperty : public TStringValue {
public:
    THostnameProperty() :
        TStringValue("hostname",
                     "Container hostname",
                     0,
                     staticProperty) {}
};

class TBindDnsProperty : public TBoolValue {
public:
    TBindDnsProperty() :
        TBoolValue("bind_dns",
                   "Bind /etc/resolv.conf and /etc/hosts of host to container",
                   0,
                   staticProperty) {}

    bool GetDefaultBool(std::shared_ptr<TContainer> c) {
        if (c->Prop->IsDefault("root"))
            return false;
        else
            return true;
    }
};

class TBindProperty : public TStringValue { // TODO: list or map
public:
    TBindProperty() :
        TStringValue("bind",
                     "Share host directories with container",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        std::vector<TBindMap> dirs;
        return ParseBind(value, dirs);
    }
};

class TNetProperty : public TStringValue { // TODO: list or map
public:
    TNetProperty() :
        TStringValue("net",
                     "Container network settings",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "host";
    }

    TError SetString(std::shared_ptr<TContainer> c,
                     std::shared_ptr<TValueState> s,
                     const std::string &value) {
        TNetCfg net;
        return ParseNet(c, value, net);
    }
};

class TAllowedDevicesProperty : public TStringValue { // TODO: list
public:
    TAllowedDevicesProperty() :
        TStringValue("allowed_devices",
                     "Devices that container can create/read/write",
                     0,
                     staticProperty) {}

    std::string GetDefaultString(std::shared_ptr<TContainer> c) {
        return "a *:* rwm";
    }
};

class TUidProperty : public TStringValue { // TODO: int
public:
    TUidProperty() : TStringValue("uid", "", HIDDEN_VALUE, {}) {}
};

class TGidProperty : public TStringValue { // TODO: int
public:
    TGidProperty() : TStringValue("gid", "", HIDDEN_VALUE, {}) {}
};

class TIdProperty : public TStringValue { // TODO: int
public:
    TIdProperty() : TStringValue("id", "", HIDDEN_VALUE, {}) {}
};

class TRootPidProperty : public TStringValue { // TODO: int
public:
    TRootPidProperty() : TStringValue("root_pid", "", HIDDEN_VALUE, {}) {}
};

TValueSpec propertySpec;
TError RegisterProperties() {
    std::vector<TValueDef *> properties = {
        new TCommandProperty,
        new TUserProperty,
        new TGroupProperty,
        new TEnvProperty,
        new TRootProperty,
        new TCwdProperty,
        new TStdinPathProperty,
        new TStdoutPathProperty,
        new TStderrPathProperty,
        new TStdoutLimitProperty,
        new TMemoryGuaranteeProperty,
        new TMemoryLimitProperty,
        new TRechargeOnPgfaultProperty,
        new TCpuPolicyProperty,
        new TCpuPriorityProperty,
        new TNetGuaranteeProperty,
        new TNetCeilProperty,
        new TNetPriorityProperty,
        new TRespawnProperty,
        new TMaxRespawnsProperty,
        new TIsolateProperty,
        new TPrivateProperty,
        new TUlimitProperty,
        new THostnameProperty,
        new TBindDnsProperty,
        new TBindProperty,
        new TNetProperty,
        new TAllowedDevicesProperty,

        new TUidProperty,
        new TGidProperty,
        new TIdProperty,
        new TRootPidProperty,
    };

    return propertySpec.Register(properties);
}

TError ParseRlimit(const std::string &s, std::map<int,struct rlimit> &rlim) {
    static const std::map<std::string,int> nameToIdx = {
        { "as", RLIMIT_AS },
        { "core", RLIMIT_CORE },
        { "cpu", RLIMIT_CPU },
        { "data", RLIMIT_DATA },
        { "fsize", RLIMIT_FSIZE },
        { "locks", RLIMIT_LOCKS },
        { "memlock", RLIMIT_MEMLOCK },
        { "msgqueue", RLIMIT_MSGQUEUE },
        { "nice", RLIMIT_NICE },
        { "nofile", RLIMIT_NOFILE },
        { "nproc", RLIMIT_NPROC },
        { "rss", RLIMIT_RSS },
        { "rtprio", RLIMIT_RTPRIO },
        { "rttime", RLIMIT_RTTIME },
        { "sigpending", RLIMIT_SIGPENDING },
        { "stask", RLIMIT_STACK },
    };

    std::vector<std::string> limits;
    TError error = SplitString(s, ';', limits);
    if (error)
        return error;

    for (auto &limit : limits) {
        std::vector<std::string> nameval;

        (void)SplitString(limit, ':', nameval);
        if (nameval.size() != 2)
            return TError(EError::InvalidValue, "Invalid limits format");

        std::string name = StringTrim(nameval[0]);
        if (nameToIdx.find(name) == nameToIdx.end())
            return TError(EError::InvalidValue, "Invalid limit " + name);
        int idx = nameToIdx.at(name);

        std::vector<std::string> softhard;
        (void)SplitString(StringTrim(nameval[1]), ' ', softhard);
        if (softhard.size() != 2)
            return TError(EError::InvalidValue, "Invalid limits number for " + name);

        rlim_t soft, hard;
        if (softhard[0] == "unlim" || softhard[0] == "unliminted") {
            soft = RLIM_INFINITY;
        } else {
            error = StringToUint64(softhard[0], soft);
            if (error)
                return TError(EError::InvalidValue, "Invalid soft limit for " + name);
        }

        if (softhard[1] == "unlim" || softhard[1] == "unliminted") {
            hard = RLIM_INFINITY;
        } else {
            error = StringToUint64(softhard[1], hard);
            if (error)
                return TError(EError::InvalidValue, "Invalid hard limit for " + name);
        }

        rlim[idx].rlim_cur = soft;
        rlim[idx].rlim_max = hard;
    }

    return TError::Success();
}

TError ParseBind(const std::string &s, std::vector<TBindMap> &dirs) {
    std::vector<std::string> lines;

    TError error = SplitEscapedString(s, ';', lines);
    if (error)
        return error;

    for (auto &line : lines) {
        std::vector<std::string> tok;
        TBindMap bindMap;

        error = SplitEscapedString(line, ' ', tok);
        if (error)
            return error;

        if (tok.size() != 2 && tok.size() != 3)
            return TError(EError::InvalidValue, "Invalid bind in: " + line);

        bindMap.Source = tok[0];
        bindMap.Dest = tok[1];
        bindMap.Rdonly = false;

        if (tok.size() == 3) {
            if (tok[2] == "ro")
                bindMap.Rdonly = true;
            else if (tok[2] == "rw")
                bindMap.Rdonly = false;
            else
                return TError(EError::InvalidValue, "Invalid bind type in: " + line);
        }

        if (!bindMap.Source.Exists())
            return TError(EError::InvalidValue, "Source bind " + bindMap.Source.ToString() + " doesn't exist");

        dirs.push_back(bindMap);
    }

    return TError::Success();
}

TError ParseNet(std::shared_ptr<const TContainer> container, const std::string &s, TNetCfg &net) {
    if (!config().network().enabled())
        return TError(EError::Unknown, "Network support is disabled");

    std::vector<std::string> lines;
    bool none = false;
    net.Share = false;

    TError error = SplitEscapedString(s, ';', lines);
    if (error)
        return error;

    if (lines.size() == 0)
        return TError(EError::InvalidValue, "Configuration is not specified");

    for (auto &line : lines) {
        if (none)
            return TError(EError::InvalidValue,
                          "none can't be mixed with other types");

        std::vector<std::string> settings;

        error = SplitEscapedString(line, ' ', settings);
        if (error)
            return error;

        if (settings.size() == 0)
            return TError(EError::InvalidValue, "Invalid net in: " + line);

        std::string type = StringTrim(settings[0]);

        if (net.Share)
            return TError(EError::InvalidValue,
                          "host can't be mixed with other settings");

        if (type == "none") {
            none = true;
        } else if (type == "host") {
            THostNetCfg hnet;

            if (settings.size() > 2)
                return TError(EError::InvalidValue, "Invalid net in: " + line);

            if (settings.size() == 1) {
                net.Share = true;
            } else {
                hnet.Dev = StringTrim(settings[1]);

                if (!ValidLink(hnet.Dev))
                    return TError(EError::InvalidValue,
                                  "Invalid host interface " + hnet.Dev);

                net.Host.push_back(hnet);
            }
        } else if (type == "macvlan") {
            if (settings.size() < 3)
                return TError(EError::InvalidValue, "Invalid macvlan in: " + line);

            std::string master = StringTrim(settings[1]);
            std::string name = StringTrim(settings[2]);
            std::string type = "bridge";
            std::string hw = "";

            if (!ValidLink(master))
                return TError(EError::InvalidValue,
                              "Invalid macvlan master " + master);


            if (settings.size() > 3) {
                type = StringTrim(settings[3]);
                if (!TNlLink::ValidMacVlanType(type))
                    return TError(EError::InvalidValue,
                                  "Invalid macvlan type " + type);
            }
            if (settings.size() > 4) {
                hw = StringTrim(settings[4]);
                if (!TNlLink::ValidMacAddr(hw))
                    return TError(EError::InvalidValue,
                                  "Invalid macvlan address " + hw);
            }

            int idx = container->GetLink()->FindIndex(master);
            if (idx < 0)
                return TError(EError::InvalidValue, "Interface " + master + " doesn't exist or not in running state");

            TMacVlanNetCfg mvlan;
            mvlan.Master = master;
            mvlan.Name = name;
            mvlan.Type = type;
            mvlan.Hw = hw;

            net.MacVlan.push_back(mvlan);
        } else {
            return TError(EError::InvalidValue, "Configuration is not specified");
        }
    }

    return TError::Success();
}
