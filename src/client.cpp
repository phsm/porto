#include <algorithm>
#include <sstream>
#include <iomanip>

#include "rpc.hpp"
#include "client.hpp"
#include "container.hpp"
#include "volume.hpp"
#include "property.hpp"
#include "statistics.hpp"
#include "config.hpp"
#include "protobuf.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "portod.hpp"
#include "event.hpp"

#include <google/protobuf/io/coded_stream.h>

extern "C" {
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
}

TClient SystemClient("<system>");
TClient WatchdogClient("<watchdog>");
__thread TClient *CL = nullptr;

TClient::TClient(int fd) : TEpollSource(fd) {
    ConnectionTime = GetCurrentTimeMs();
    ActivityTimeMs = ConnectionTime;
    if (fd >= 0)
        Statistics->ClientsCount++;
}

TClient::TClient(const std::string &special) {
    Cred = TCred(RootUser, RootGroup);
    TaskCred = TCred(RootUser, RootGroup);
    Comm = special;
    AccessLevel = EAccessLevel::Internal;
}

TClient::~TClient() {
    CloseConnection();
}

void TClient::CloseConnection() {
    TScopedLock lock(Mutex);

    if (Fd >= 0) {
        if (InEpoll)
            EpollLoop->RemoveSource(Fd);
        ConnectionTime = GetCurrentTimeMs() - ConnectionTime;
        L_VERBOSE("Disconnected {} time={} ms", Id, ConnectionTime);
        close(Fd);
        Fd = -1;

        Statistics->ClientsCount--;
        if (ClientContainer)
            ClientContainer->ClientsCount--;
    }

    for (auto &weakCt: WeakContainers) {
        auto ct = weakCt.lock();
        if (ct && ct->IsWeak) {
            TEvent ev(EEventType::DestroyWeakContainer, ct);
            EventQueue->Add(0, ev);
        }
    }
    WeakContainers.clear();
}

void TClient::StartRequest() {
    RequestTimeMs = GetCurrentTimeMs();
    ActivityTimeMs = RequestTimeMs;
    PORTO_ASSERT(CL == nullptr);
    CL = this;
}

void TClient::FinishRequest() {
    RequestTimeMs = GetCurrentTimeMs() - RequestTimeMs;
    ReleaseContainer();
    PORTO_ASSERT(CL == this);
    CL = nullptr;
}

TError TClient::IdentifyClient(bool initial) {
    std::shared_ptr<TContainer> ct;
    struct ucred cr;
    socklen_t len = sizeof(cr);
    TError error;

    if (getsockopt(Fd, SOL_SOCKET, SO_PEERCRED, &cr, &len))
        return TError::System("Cannot identify client: getsockopt() failed");

    /* check that request from the same pid and container is still here */
    if (!initial && Pid == cr.pid && TaskCred.Uid == cr.uid &&
            TaskCred.Gid == cr.gid && ClientContainer &&
            (ClientContainer->State == EContainerState::Running ||
             ClientContainer->State == EContainerState::Starting ||
             ClientContainer->State == EContainerState::Meta))
        return OK;

    TaskCred.Uid = cr.uid;
    TaskCred.Gid = cr.gid;
    Pid = cr.pid;

    Cred = TaskCred;
    Comm = GetTaskName(Pid);

    error = TContainer::FindTaskContainer(Pid, ct);
    if (error && error.Errno != ENOENT)
        L_WRN("Cannot identify container of pid {} : {}", Pid, error);

    if (error)
        return error;

    AccessLevel = ct->AccessLevel;
    for (auto p = ct->Parent; p; p = p->Parent)
        AccessLevel = std::min(AccessLevel, p->AccessLevel);

    PortoNamespace = ct->GetPortoNamespace();
    WriteNamespace = ct->GetPortoNamespace(true);

    if (AccessLevel == EAccessLevel::None)
        return TError(EError::Permission, "Porto disabled in container " + ct->Name);

    if (ct->State != EContainerState::Running &&
            ct->State != EContainerState::Starting &&
            ct->State != EContainerState::Meta)
        return TError(EError::Permission, "Client from containers in state " + TContainer::StateName(ct->State));

    if (ct->ClientsCount < 0)
        L_ERR("Client count underflow");

    if (ClientContainer)
        ClientContainer->ClientsCount--;
    ClientContainer = ct;
    ct->ClientsCount++;

    /* requests from containers are executed in behalf of their owners */
    if (!ct->IsRoot())
        Cred = ct->OwnerCred;

    (void)Cred.InitGroups(Cred.User());

    if (Cred.IsRootUser()) {
        if (AccessLevel == EAccessLevel::Normal)
            AccessLevel = EAccessLevel::SuperUser;
    } else if (!Cred.IsMemberOf(PortoGroup)) {
        if (AccessLevel > EAccessLevel::ReadOnly)
            AccessLevel = EAccessLevel::ReadOnly;
    }

    Id = fmt::format("CL{}:{}({}) CT{}:{}", Fd, Comm, Pid, ct->Id, ct->Name);

    L_VERBOSE("Connected {} cred={} tcred={} access={} ns={} wns={}",
                Id, Cred.ToString(), TaskCred.ToString(),
                AccessLevel <= EAccessLevel::ReadOnly ? "ro" : "rw",
                PortoNamespace, WriteNamespace);

    return OK;
}

std::string TClient::RelativeName(const std::string &name) const {
    if (name == ROOT_CONTAINER)
        return ROOT_CONTAINER;
    if (PortoNamespace == "")
        return name;
    if (StringStartsWith(name, PortoNamespace))
        return name.substr(PortoNamespace.length());
    return ROOT_PORTO_NAMESPACE + name;
}

TError TClient::ComposeName(const std::string &name, std::string &relative_name) const {
    if (name == ROOT_CONTAINER) {
        relative_name = ROOT_CONTAINER;
        return OK;
    }

    if (PortoNamespace == "") {
        relative_name = name;
        return OK;
    }

    if (!StringStartsWith(name, PortoNamespace))
        return TError(EError::Permission,
                "Cannot access container " + name + " from namespace " + PortoNamespace);

    relative_name = name.substr(PortoNamespace.length());
    return OK;
}

TError TClient::ResolveName(const std::string &relative_name, std::string &name) const {
    if (relative_name == ROOT_CONTAINER)
        name = ROOT_CONTAINER;
    else if (relative_name == SELF_CONTAINER)
        name = ClientContainer->Name;
    else if (relative_name == DOT_CONTAINER)
        name = TContainer::ParentName(PortoNamespace);
    else if (StringStartsWith(relative_name, SELF_CONTAINER + std::string("/"))) {
        name = relative_name.substr(strlen(SELF_CONTAINER) + 1);
        if (!ClientContainer->IsRoot())
            name = ClientContainer->Name + "/" + name;
    } else if (StringStartsWith(relative_name, ROOT_PORTO_NAMESPACE))
        name = relative_name.substr(strlen(ROOT_PORTO_NAMESPACE));
    else
        name = PortoNamespace + relative_name;

    name = TPath(name).NormalPath().ToString();
    if (name == ".")
        name = ROOT_CONTAINER;

    if (StringStartsWith(name, PortoNamespace) ||
            StringStartsWith(name, ClientContainer->Name + "/") ||
            StringStartsWith(ClientContainer->Name + "/", name + "/") ||
            name == ROOT_CONTAINER)
        return OK;

    return TError(EError::Permission, "container name out of namespace: " + relative_name);
}

TError TClient::ResolveContainer(const std::string &relative_name,
                                 std::shared_ptr<TContainer> &ct) const {
    std::string name;
    TError error = ResolveName(relative_name, name);
    if (error)
        return error;
    return TContainer::Find(name, ct);
}

TError TClient::ResolveVolume(const TPath &path, std::shared_ptr<TVolume> &volume) {
    volume = TVolume::ResolveMount(ClientContainer->RootPath / path);
    if (!volume)
        return TError(EError::VolumeNotFound, "Volume {} not found", path);
    return OK;
}

TError TClient::ControlVolume(const TPath &path, std::shared_ptr<TVolume> &volume) {
    if (AccessLevel <= EAccessLevel::ReadOnly)
        return TError(EError::Permission, "Write access denied");
    volume = TVolume::ResolveMount(ClientContainer->RootPath / path);
    if (!volume)
        return TError(EError::VolumeNotFound, "Volume {} not found", path);
    if (CanControl(volume->VolumeOwner))
        return TError(EError::Permission, "Cannot control volume {}", path);
    return OK;
}

TError TClient::ReadContainer(const std::string &relative_name,
                              std::shared_ptr<TContainer> &ct, bool try_lock) {
    auto lock = LockContainers();
    TError error = ResolveContainer(relative_name, ct);
    if (error)
        return error;
    ReleaseContainer(true);
    error = ct->LockRead(lock, try_lock);
    if (error)
        return error;
    LockedContainer = ct;
    return OK;
}

TError TClient::WriteContainer(const std::string &relative_name,
                               std::shared_ptr<TContainer> &ct, bool child) {
    if (AccessLevel <= EAccessLevel::ReadOnly)
        return TError(EError::Permission, "Write access denied");
    auto lock = LockContainers();
    TError error = ResolveContainer(relative_name, ct);
    if (error)
        return error;
    error = CanControl(*ct, child);
    if (error)
        return error;
    ReleaseContainer(true);
    error = ct->Lock(lock);
    if (error)
        return error;
    LockedContainer = ct;
    return OK;
}

TError TClient::LockContainer(std::shared_ptr<TContainer> &ct) {
    auto lock = LockContainers();
    ReleaseContainer(true);
    TError error = ct->Lock(lock);
    if (!error)
        LockedContainer = ct;
    return error;
}

void TClient::ReleaseContainer(bool locked) {
    if (LockedContainer) {
        LockedContainer->Unlock(locked);
        LockedContainer = nullptr;
    }
}

TPath TClient::ComposePath(const TPath &path) {
    return ClientContainer->RootPath.InnerPath(path);
}

TPath TClient::ResolvePath(const TPath &path) {
    return ClientContainer->RootPath / path;
}

TPath TClient::DefaultPlace() {
    if (ClientContainer->Place.size())
        return ClientContainer->Place[0];
    return PORTO_PLACE;
}

TError TClient::CanControlPlace(const TPath &place) {
    for (auto &mask: ClientContainer->Place)
        if (StringMatch(place.ToString(), mask))
            return OK;
    return TError(EError::Permission, "You are not permitted to use place " + place.ToString());
}

bool TClient::IsSuperUser(void) const {
    return AccessLevel >= EAccessLevel::SuperUser;
}

bool TClient::CanSetUidGid() const {
    /* loading capabilities by pid is racy, use container limits instead */
    if (TaskCred.IsRootUser())
        return ClientContainer->CapBound.HasSetUidGid();
    return ClientContainer->CapAmbient.HasSetUidGid();
}

TError TClient::CanControl(const TCred &other) {

    if (AccessLevel <= EAccessLevel::ReadOnly)
        return TError(EError::Permission, "Write access denied");

    if (IsSuperUser() || Cred.Uid == other.Uid || other.IsUnknown())
        return OK;

    /* Everybody can control users from group porto-containers */
    if (other.IsMemberOf(PortoCtGroup))
        return OK;

    /* Init group $USER-containers lazily */
    if (!UserCtGroup && GroupId(Cred.User() + USER_CT_SUFFIX, UserCtGroup))
        UserCtGroup = NoGroup;

    if (other.IsMemberOf(UserCtGroup))
        return OK;

    return TError(EError::Permission, Cred.ToString() + " cannot control " + other.ToString());
}

TError TClient::CanControl(const TContainer &ct, bool child) {

    if (AccessLevel <= EAccessLevel::ReadOnly)
        return TError(EError::Permission, "Write access denied");

    if (!child && ct.IsRoot())
        return TError(EError::Permission, "Write access denied: root container is read-only");

    /*
     * Container must be in write namespace or be its base for new childs.
     * Also allow write access to client subcontainers for self/... notation.
     * Self-isolate allows write access to self.
     */
    if (StringStartsWith(ct.Name, WriteNamespace) ||
        (child && ct.Name == TContainer::ParentName(WriteNamespace)) ||
        ct.IsChildOf(*ClientContainer) ||
        (&ct == &*ClientContainer &&
         (child || ct.AccessLevel == EAccessLevel::SelfIsolate))) {

        /* Everybody can create first level containers */
        if (!(child && ct.IsRoot())) {
            TError error = CanControl(ct.OwnerCred);
            if (error)
                return TError(error, "Write access denied: container {}", ct.Name);
        }

        return OK;
    }

    return TError(EError::Permission, "Write access denied: container {} out of scope", ct.Name);
}

TError TClient::ReadAccess(const TFile &file) {
    TError error = file.ReadAccess(TaskCred);

    /* Without chroot read access to file is enough */
    if (!error && ClientContainer->RootPath.IsRoot())
        return error;

    /* Check that real path is inside chroot */
    TPath path = file.RealPath();
    if (!path.IsInside(ClientContainer->RootPath))
        return TError(EError::Permission, "Path out of chroot " + path.ToString());

    /* Volume owner also gains full control inside */
    if (error) {
        auto volume = TVolume::ResolveOrigin(path);
        if (volume && !CanControl(volume->VolumeOwner))
            error = OK;
    }

    return error;
}

TError TClient::WriteAccess(const TFile &file) {
    TError error = file.WriteAccess(TaskCred);

    /* Without chroot write access to file is enough */
    if (!error && ClientContainer->RootPath.IsRoot())
        return error;

    /* Check that real path is inside chroot */
    TPath path = file.RealPath();
    if (!path.IsInside(ClientContainer->RootPath))
        return TError(EError::Permission, "Path out of chroot " + path.ToString());

    /* Inside chroot everybody gain root access but fs might be read-only */
    if (error && ClientContainer->RootPath.IsRoot())
        error = file.WriteAccess(TCred(RootUser, RootGroup));

    /* Also volume owner gains full access inside */
    if (error) {
        auto volume = TVolume::ResolveOrigin(path);
        if (volume && !volume->IsReadOnly &&
                !CanControl(volume->VolumeOwner))
            error = OK;
    }

    return error;
}

TError TClient::ReadRequest(rpc::TContainerRequest &request) {
    TScopedLock lock(Mutex);

    if (Processing) {
        L_WRN("{} request before response", Id);
        return OK;
    }

    if (Fd < 0)
        return TError("Connection closed");

    if (Offset >= Buffer.size())
        Buffer.resize(Offset + 4096);

    ssize_t len = recv(Fd, &Buffer[Offset], Buffer.size() - Offset, MSG_DONTWAIT);
    if (len > 0)
        Offset += len;
    else if (len == 0)
        return TError("recv return zero");
    else if (errno != EAGAIN && errno != EWOULDBLOCK)
        return TError::System("recv request failed");

    ActivityTimeMs = GetCurrentTimeMs();

    if (Length && Offset < Length)
        return TError::Queued();

    google::protobuf::io::CodedInputStream input(&Buffer[0], Offset);

    uint32_t length;
    if (!input.ReadVarint32(&length))
        return TError::Queued();

    if (!Length) {
        if (length > config().daemon().max_msg_len())
            return TError("oversized request: {}", length);

        Length = length + google::protobuf::io::CodedOutputStream::VarintSize32(length);
        if (Buffer.size() < Length)
            Buffer.resize(Length + 4096);

        if (Offset < Length)
            return TError::Queued();
    }

    if (!request.ParseFromCodedStream(&input))
        return TError("cannot parse request");

    if (Offset > Length)
        return TError("garbage after request");

    WaitRequest = request.has_wait();
    Offset = 0;
    Processing = true;

    return EpollLoop->StopInput(Fd);
}

TError TClient::SendResponse(bool first) {
    TScopedLock lock(Mutex);

    if (Fd < 0)
        return OK; /* Connection closed */

    ssize_t len = send(Fd, &Buffer[Offset], Length - Offset, MSG_DONTWAIT);
    if (len > 0)
        Offset += len;
    else if (len == 0) {
        if (!first)
            return TError("send return zero");
    } else if (errno == EPIPE) {
        L_VERBOSE("Disconnected {}", Id);
        return OK;
    } else if (errno != EAGAIN && errno != EWOULDBLOCK)
        return TError::System("send response failed");

    ActivityTimeMs = GetCurrentTimeMs();

    if (Offset >= Length) {
        Length = Offset = 0;
        Processing = false;

        if (ShutdownPortod && shutdown(Fd, SHUT_RDWR))
            L_ERR("Cannot shutdown client: {}", TError::System("shutdown"));

        return EpollLoop->StartInput(Fd);
    }

    if (first)
        return EpollLoop->StartOutput(Fd);

    return OK;
}

TError TClient::QueueResponse(rpc::TContainerResponse &response) {
    uint32_t length = response.ByteSize();
    size_t lengthSize = google::protobuf::io::CodedOutputStream::VarintSize32(length);

    Offset = 0;
    Length = lengthSize + length;

    if (Buffer.size() < Length)
        Buffer.resize(Length);

    google::protobuf::io::CodedOutputStream::WriteVarint32ToArray(length, &Buffer[0]);
    if (!response.SerializeToArray(&Buffer[lengthSize], length))
        return TError("cannot serialize response");

    return SendResponse(true);
}
