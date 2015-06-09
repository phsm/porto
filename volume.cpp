#include <memory>
#include <sstream>
#include <algorithm>

#include "volume.hpp"
#include "container.hpp"
#include "holder.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/folder.hpp"
#include "util/unix.hpp"
#include "util/sha256.hpp"
#include "config.hpp"

extern "C" {
#include <sys/vfs.h>
#include <sys/mount.h>
#include "util/ext4_proj_quota.h"
}

/* TVolumeBackend - abstract */

TError TVolumeBackend::Configure() {
    return TError::Success();
}

TError TVolumeBackend::Clear() {
    return Volume->GetPath().ClearDirectory();
}

TError TVolumeBackend::Save(std::shared_ptr<TValueMap> Config) {
    return TError::Success();
}

TError TVolumeBackend::Restore(std::shared_ptr<TValueMap> Config) {
    return TError::Success();
}

TError TVolumeBackend::Resize(uint64_t space_limit, uint64_t inode_limit) {
    return TError(EError::NotSupported, "not implemented");
}

TError TVolumeBackend::Move(TPath) {
    return TError(EError::NotSupported, "not implemented");
}

TError TVolumeBackend::GetStat(uint64_t &space_used, uint64_t &space_avail,
                               uint64_t &inode_used, uint64_t &inode_avail) {
    return Volume->GetPath().StatVFS(space_used, space_avail,
                                     inode_used, inode_avail);
}

/* TVolumePlainBackend - bindmount */

class TVolumePlainBackend : public TVolumeBackend {
public:
    TVolumePlainBackend(std::shared_ptr<TVolume> volume) : TVolumeBackend(volume) {}

    TError Build() override {
        TPath storage = Volume->GetStorage();

        TError error = storage.Chown(Volume->GetCred());
        if (error)
            return error;

        error = storage.Chmod(Volume->GetPermissions());
        if (error)
            return error;

        TMount Mount = TMount(storage, Volume->GetPath(), "none", {});
        return Mount.Bind(Volume->IsReadOnly());
    }

    TError Clear() override {
        return Volume->GetStorage().ClearDirectory();
    }

    TError Destroy() override {
        auto storage = Volume->GetStorage();
        TMount Mount(storage, Volume->GetPath(), "none", {});
        TError error = Mount.Umount();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;

        return TError::Success();
    }

    TError Move(TPath dest) override {
        TMount mount(Volume->GetStorage(), Volume->GetPath(), "none", {});
        return mount.Move(dest);
    }
};

/* TVolumeNativeBackend - project quota + bindmount */

class TVolumeNativeBackend : public TVolumeBackend {
public:
    TVolumeNativeBackend(std::shared_ptr<TVolume> volume) : TVolumeBackend(volume) {}

    TError Build() override {
        TPath storage = Volume->GetStorage();
        uint64_t space, inodes;

        Volume->GetQuota(space, inodes);
        if (ext4_create_project(storage.c_str(), space, inodes))
            return TError(EError::Unknown, errno, "ext4_create_project");

        TError error = storage.Chown(Volume->GetCred());
        if (error)
            return error;

        error = storage.Chmod(Volume->GetPermissions());
        if (error)
            return error;

        TMount Mount = TMount(storage, Volume->GetPath(), "none", {});
        return Mount.Bind(Volume->IsReadOnly());
    }

    TError Clear() override {
        return Volume->GetStorage().ClearDirectory();
    }

    TError Destroy() override {
        auto storage = Volume->GetStorage();
        TMount Mount(storage, Volume->GetPath(), "none", {});
        TError error = Mount.Umount();
        if (error)
            L_ERR() << "Can't umount volume: " << error << std::endl;

        if (ext4_destroy_project(storage.c_str()))
            L_ERR() << "Can't destroy ext4 project: " << errno << std::endl;

        return TError::Success();
    }

    TError Move(TPath dest) override {
        TMount mount(Volume->GetStorage(), Volume->GetPath(), "none", {});
        return mount.Move(dest);
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        if (ext4_resize_project(Volume->GetStorage().c_str(),
                                space_limit, inode_limit))
            TError(EError::Unknown, errno, "ext4_resize_project");
        return TError::Success();
    }
};

/* TVolumeLoopBackend - ext4 image + loop device */

class TVolumeLoopBackend : public TVolumeBackend {
    int LoopDev = -1;

public:
    TVolumeLoopBackend(std::shared_ptr<TVolume> volume) : TVolumeBackend(volume) { }

    TPath GetLoopImage() {
        return Volume->GetStorage().AddComponent("loop.img");
    }

    TError Save(std::shared_ptr<TValueMap> Config) override {
        return Config->Set<int>(V_LOOP_DEV, LoopDev);
    }

    TError Restore(std::shared_ptr<TValueMap> Config) override {
        LoopDev = Config->Get<int>(V_LOOP_DEV);
        return TError::Success();
    }

    TError Build() override {
        TPath path = Volume->GetPath();
        TPath image = GetLoopImage();
        uint64_t space_limit, inode_limit;
        TError error;

        Volume->GetQuota(space_limit, inode_limit);
        if (!space_limit)
            return TError(EError::InvalidValue, "loop backend requires space_limit");

        error = GetLoopDev(LoopDev);
        if (error)
            return error;

        TLoopMount m = TLoopMount(image, path, "ext4", LoopDev);

        if (!image.Exists()) {
            L_ACT() << "Allocate loop image with size " << space_limit << std::endl;
            error = AllocLoop(image, space_limit);
            if (error)
                goto put_loop;
        } else {
            //FIXME call resize2fs
        }

        error = m.Mount();
        if (error)
            goto put_loop;

        error = path.Chown(Volume->GetCred());
        if (error)
            goto umount_loop;

        error = path.Chmod(Volume->GetPermissions());
        if (error)
            goto umount_loop;

        return TError::Success();

umount_loop:
        (void)m.Umount();
        LoopDev = -1;
        return error;

put_loop:
        if (LoopDev != -1)
            PutLoopDev(LoopDev);
        LoopDev = -1;
        return error;
    }

    TError Destroy() override {
        L_ACT() << "Destroy loop " << LoopDev << std::endl;
        TLoopMount m = TLoopMount(GetLoopImage(), Volume->GetPath(), "ext4", LoopDev);
        return m.Umount();
    }

    TError Clear() override {
        return Volume->GetPath().ClearDirectory();
    }

    TError Move(TPath dest) override {
        TMount mount(GetLoopImage(), Volume->GetPath(), "ext4", {});
        return mount.Move(dest);
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        return TError(EError::NotSupported, "loop backend doesn't suppport resize");
    }
};

/* TVolumeOverlayBackend - project quota + overlayfs */

class TVolumeOverlayBackend : public TVolumeBackend {
public:
    TVolumeOverlayBackend(std::shared_ptr<TVolume> volume) : TVolumeBackend(volume) {}

    TError Build() override {
        TPath storage = Volume->GetStorage();
        TPath upper = storage.AddComponent("upper");
        TPath work = storage.AddComponent("work");
        uint64_t space_limit, inode_limit;
        TError error;
        std::stringstream lower;
        int index = 0;

        for (auto layer: Volume->GetOverlays()) {
            if (index++)
                lower << ":";
            lower << layer;
        }

        TMount mount("overlay", Volume->GetPath(), "overlay",
                     { "lowerdir=" + lower.str(),
                       "upperdir=" + upper.ToString(),
                       "workdir=" + work.ToString() });

        Volume->GetQuota(space_limit, inode_limit);
        if (ext4_create_project(storage.c_str(),
                                space_limit, inode_limit))
            return TError(EError::Unknown, errno, "ext4_create_project");

        error = upper.Mkdir(0755);
        if (error)
            goto err;

        error = work.Mkdir(0755);
        if (error)
            goto err;

        error = upper.Chown(Volume->GetCred());
        if (error)
            goto err;

        error = upper.Chmod(Volume->GetPermissions());
        if (error)
            return error;

        error = mount.Mount(Volume->IsReadOnly() ? MS_RDONLY : 0);
        if (!error)
            return error;
err:
        (void)ext4_destroy_project(storage.c_str());
        return error;
    }

    TError Clear() override {
        return Volume->GetStorage().AddComponent("upper").ClearDirectory();
    }

    TError Destroy() override {
        TPath storage = Volume->GetStorage();
        TMount mount("overlay", Volume->GetPath(), "overlay", {});
        TError error = mount.Umount();
        if (error)
            L_ERR() << "Can't umount overlay: " << error << std::endl;

        error = storage.ClearDirectory();
        if (error)
            L_ERR() << "Can't clear overlay storage: " << error << std::endl;

        if (ext4_destroy_project(storage.c_str()))
            L_ERR() << "Can't destroy ext4 project: " << errno << std::endl;

        return TError::Success();
    }

    TError Move(TPath dest) override {
        TMount mount("overlay", Volume->GetPath(), "overlay", {});
        return mount.Move(dest);
    }

    TError Resize(uint64_t space_limit, uint64_t inode_limit) override {
        if (ext4_resize_project(Volume->GetStorage().c_str(),
                                space_limit, inode_limit))
            TError(EError::Unknown, errno, "ext4_resize_project");
        return TError::Success();
    }
};

/* TVolume */

TError TVolume::OpenBackend() {
    if (GetBackend() == "plain")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumePlainBackend(shared_from_this()));
    else if (GetBackend() == "native")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeNativeBackend(shared_from_this()));
    else if (GetBackend() == "overlay")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeOverlayBackend(shared_from_this()));
    else if (GetBackend() == "loop")
        Backend = std::unique_ptr<TVolumeBackend>(new TVolumeLoopBackend(shared_from_this()));
    else
        return TError(EError::InvalidValue, "Unknown volume backend: " + GetBackend());

    return TError::Success();
}

/* /place/porto_volumes/<id>/<type> */
TPath TVolume::GetInternal(std::string type) const {
    return TPath(config().volumes().volume_dir()).AddComponent(std::to_string(GetId())).AddComponent(type);
}

TPath TVolume::GetPath() const {
    auto val = Config->Find(V_PATH);
    if (val->HasValue())
        return val->Get<std::string>();
    else
        return GetInternal("volume");
}

TPath TVolume::GetStorage() const {
    auto val = Config->Find(V_STORAGE);
    if (val->HasValue())
        return val->Get<std::string>();
    else
        return GetInternal(GetBackend());
}

TError TVolume::Configure(const TPath &path, const TCred &creator_cred,
                          std::shared_ptr<TContainer> creator_container,
                          const std::map<std::string, std::string> &properties) {
    TError error;

    /* Verify volume path */
    if (!path.IsEmpty()) {
        if (!path.IsAbsolute())
            return TError(EError::InvalidValue, "Volume path must be absolute");
        if (path.GetType() != EFileType::Directory)
            return TError(EError::InvalidValue, "Volume path must be a directory");
        if (!path.AccessOk(EFileAccess::Write, creator_cred))
            return TError(EError::Permission, "Volume path usage not permitted");
        error = Config->Set<std::string>(V_PATH, path.ToString());
        if (error)
            return error;
    }

    /* Verify storage path */
    if (properties.count(V_STORAGE)) {
        TPath storage(properties.at(V_STORAGE));
        if (!storage.IsAbsolute())
            return TError(EError::InvalidValue, "Storage path must be absolute");
        if (storage.GetType() != EFileType::Directory)
            return TError(EError::InvalidValue, "Storage path must be a directory");
        if (!storage.AccessOk(EFileAccess::Write, creator_cred))
            return TError(EError::Permission, "Storage path usage not permitted");
    }

    /* Save original creator. Just for the record. */
    error = Config->Set<std::string>(V_CREATOR, creator_container->GetName() + " " +
                    creator_cred.UserAsString() + " " + creator_cred.GroupAsString());
    if (error)
        return error;

    /* Set default credentials to creator */
    error = Config->Set<std::string>(V_USER, creator_cred.UserAsString());
    if (error)
        return error;
    error = Config->Set<std::string>(V_GROUP, creator_cred.GroupAsString());
    if (error)
        return error;

    /* Default permissions for volume root directory */
    error = Config->Set<std::string>(V_PERMISSIONS, "0775");
    if (error)
        return error;

    /* Apply properties */
    for (auto p: properties) {
        if (!Config->IsValid(p.first))
            return TError(EError::InvalidValue, "Invalid volume property: " + p.first);
        if (Config->IsReadOnly(p.first))
            return TError(EError::InvalidValue, "Read-only volume property: " + p.first);
        error = Config->FromString(p.first, p.second);
        if (error)
            return error;
    }

    error = Cred.Parse(Config->Get<std::string>(V_USER),
                       Config->Get<std::string>(V_GROUP));
    if (error)
        return error;

    /* Verify default credentials */
    if (Cred.Uid != creator_cred.Uid && !creator_cred.IsPrivileged())
        return TError(EError::Permission, "Changing user is not permitted");

    if (Cred.Gid != creator_cred.Gid && !creator_cred.IsPrivileged() &&
            !creator_cred.MemberOf(Cred.Gid))
        return TError(EError::Permission, "Changing group is not permitted");

    /* Verify default permissions */
    error = StringToOct(Config->Get<std::string>(V_PERMISSIONS), Permissions);
    if (error)
        return error;

    /* Autodetect volume backend */
    if (!Config->HasValue(V_BACKEND)) {
        if (Config->HasValue(V_OVERLAYS)) {
            if (!config().volumes().native()) //FIXME
                return TError(EError::InvalidValue, "overlay not supported");
            error = Config->Set<std::string>(V_BACKEND, "overlay");
        } else if (config().volumes().native())
            error = Config->Set<std::string>(V_BACKEND, "native");
        else if (Config->HasValue(V_SPACE_LIMIT) ||
                 Config->HasValue(V_INODE_LIMIT))
            error = Config->Set<std::string>(V_BACKEND, "loop");
        else
            error = Config->Set<std::string>(V_BACKEND, "plain");
        if (error)
            return error;
    }

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Configure();
    if (error)
        return error;

    return TError::Success();
}

TError TVolume::Build() {
    TPath storage = GetStorage();
    TPath path = GetPath();
    TPath internal = GetInternal("");

    L_ACT() << "Build volume " << GetPath() << std::endl;

    TError error = internal.Mkdir(0755);
    if (error)
        goto err_internal;

    if (!Config->HasValue(V_STORAGE)) {
        error = storage.Mkdir(0755);
        if (error)
            goto err_storage;
    }

    if (!Config->HasValue(V_PATH)) {
        error = path.Mkdir(0755);
        if (error)
            goto err_path;
    }

    error = Backend->Build();
    if (!error)
        return error;

    if (!Config->HasValue(V_PATH))
        (void)path.Rmdir();
err_path:
    if (!Config->HasValue(V_STORAGE))
        (void)storage.Rmdir();
err_storage:
    (void)internal.Rmdir();
err_internal:
    return error;
}

TError TVolume::Clear() {
    L_ACT() << "Clear volume " << GetPath() << std::endl;
    return Backend->Clear();
}

TError TVolume::Destroy() {
    TPath internal = GetInternal("");
    TPath storage = GetStorage();
    TPath path = GetPath();
    TError ret = TError::Success(), error;

    L_ACT() << "Destroy volume " << GetPath() << std::endl;

    if (Backend) {
        error = Backend->Destroy();
        if (error) {
            L_ERR() << "Can't destroy volume backend: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (!Config->HasValue(V_STORAGE) && storage.Exists()) {
        error = storage.ClearDirectory();
        if (error) {
            L_ERR() << "Can't clear storage: " << error << std::endl;
            if (!ret)
                ret = error;
        }

        error = storage.Rmdir();
        if (error) {
            L_ERR() << "Can't remove storage: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (!Config->HasValue(V_PATH) && path.Exists()) {
        error = path.Rmdir();
        if (error) {
            L_ERR() << "Can't remove volume path: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (internal.Exists()) {
        error = internal.ClearDirectory();
        if (error) {
            L_ERR() << "Can't clear internal: " << error << std::endl;
            if (!ret)
                ret = error;
        }

        error = internal.Rmdir();
        if (error) {
            L_ERR() << "Can't remove internal: " << error << std::endl;
            if (!ret)
                ret = error;
        }
    }

    if (Holder)
        Holder->Unregister(shared_from_this());

    if (Config)
        Config->Remove();

    return ret;
}

TError TVolume::GetStat(uint64_t &space_used, uint64_t &space_avail,
                        uint64_t &inode_used, uint64_t &inode_avail) const {
    return Backend->GetStat(space_used, space_avail, inode_used, inode_avail);
}

TError TVolume::Resize(uint64_t space_limit, uint64_t inode_limit) {
    L_ACT() << "Resize volume " << GetPath() << " to " << space_limit << " " << inode_limit << std::endl;
    TError error = Backend->Resize(space_limit, inode_limit);
    if (error)
        return error;
    Config->Set<uint64_t>(V_SPACE_LIMIT, space_limit);
    Config->Set<uint64_t>(V_INODE_LIMIT, inode_limit);
    return TError::Success();
}

TError TVolume::LinkContainer(std::string name) {
    std::vector<std::string> containers(Config->Get<std::vector<std::string>>(V_CONTAINERS));
    containers.push_back(name);
    return Config->Set<std::vector<std::string>>(V_CONTAINERS, containers);
}

TError TVolume::UnlinkContainer(std::string name) {
    auto containers(Config->Get<std::vector<std::string>>(V_CONTAINERS));
    containers.erase(std::remove(containers.begin(), containers.end(), name), containers.end());
    TError error = Config->Set<std::vector<std::string>>(V_CONTAINERS, containers);
    if (!error && containers.empty()) {
        error = SetReady(false);
        error = Destroy(); //FIXME later
    }
    return error;
}

std::map<std::string, std::string> TVolume::GetProperties() {
    uint64_t space_used, space_avail, inode_used, inode_avail;
    std::map<std::string, std::string> ret;

    if (IsReady() && !GetStat(space_used, space_avail, inode_used, inode_avail)) {
        Config->Set<uint64_t>(V_SPACE_USED, space_used);
        Config->Set<uint64_t>(V_INODE_USED, inode_used);
        Config->Set<uint64_t>(V_SPACE_AVAILABLE, space_avail);
        Config->Set<uint64_t>(V_INODE_AVAILABLE, inode_avail);
    }

    for (auto name: Config->List()) {
        auto property = Config->Find(name);
        if (!(property->GetFlags() & HIDDEN_VALUE) && property->HasValue())
            ret[name] = property->ToString();
    }

    return ret;
}

TError TVolume::CheckPermission(const TCred &ucred) const {
    if (ucred.IsPrivileged())
        return TError::Success();

    if (Cred == ucred)
        return TError::Success();

    if (ucred.MemberOf(Cred.Gid))
        return TError::Success();

    return TError(EError::Permission, "Permission denied");
}

TError TVolume::Restore() {
    if (!IsReady())
        return TError(EError::VolumeNotReady, "Volume not ready");

    TError error = Cred.Parse(Config->Get<std::string>(V_USER),
                              Config->Get<std::string>(V_GROUP));
    if (error)
        return TError(EError::InvalidValue, "Bad volume " + GetPath().ToString() + " credentials: " +
                      Config->Get<std::string>(V_USER) + " " +
                      Config->Get<std::string>(V_GROUP));

    error = StringToOct(Config->Get<std::string>(V_PERMISSIONS), Permissions);
    if (error)
        return error;

    error = OpenBackend();
    if (error)
        return error;

    error = Backend->Restore(Config);
    if (error)
        return error;

    return TError::Success();
}

/* TVolumeHolder */

const std::vector<std::pair<std::string, std::string>> TVolumeHolder::ListProperties() {
    return {
        { V_BACKEND,     "plain|native|loop|overlay  default - autodetect" },
        { V_STORAGE,     "path to data storage" },
        { V_READY,       "true|false (readonly)" },
        { V_USER,        "user  default - creator" },
        { V_GROUP,       "group  default - creator" },
        { V_PERMISSIONS, "directory permissions  default - 0775" },
        { V_CREATOR,     "container user group" },
        { V_READ_ONLY,   "true|false  default - false" },
        { V_OVERLAYS,    "top-layer;...;bottom-layer  overlay layers" },
        { V_SPACE_LIMIT, " " },
        { V_INODE_LIMIT, " " },
        //{ V_SPACE_GUARANTEE, " " },
        //{ V_INODE_GUARANTEE, " " },
        { V_SPACE_USED, " " },
        { V_INODE_USED, " " },
        { V_SPACE_AVAILABLE, " " },
        { V_INODE_AVAILABLE, " " },
    };
}

static void RegisterVolumeProperties(std::shared_ptr<TRawValueMap> m) {
    m->Add(V_PATH, new TStringValue(HIDDEN_VALUE | PERSISTENT_VALUE));
    m->Add(V_STORAGE, new TStringValue(PERSISTENT_VALUE));

    m->Add(V_BACKEND, new TStringValue(PERSISTENT_VALUE));

    m->Add(V_USER, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_GROUP, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_PERMISSIONS, new TStringValue(PERSISTENT_VALUE));
    m->Add(V_CREATOR, new TStringValue(READ_ONLY_VALUE | PERSISTENT_VALUE));

    m->Add(V_ID, new TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE));
    m->Add(V_READY, new TBoolValue(READ_ONLY_VALUE | PERSISTENT_VALUE));
    m->Add(V_CONTAINERS, new TListValue(HIDDEN_VALUE | PERSISTENT_VALUE));

    m->Add(V_LOOP_DEV, new TIntValue(HIDDEN_VALUE | PERSISTENT_VALUE));
    m->Add(V_READ_ONLY, new TBoolValue(PERSISTENT_VALUE));
    m->Add(V_OVERLAYS, new TListValue(PERSISTENT_VALUE));

    m->Add(V_SPACE_LIMIT, new TUintValue(PERSISTENT_VALUE | UINT_UNIT_VALUE));
    m->Add(V_INODE_LIMIT, new TUintValue(PERSISTENT_VALUE | UINT_UNIT_VALUE));

    m->Add(V_SPACE_GUARANTEE, new TUintValue(PERSISTENT_VALUE | UINT_UNIT_VALUE));
    m->Add(V_INODE_GUARANTEE, new TUintValue(PERSISTENT_VALUE | UINT_UNIT_VALUE));

    m->Add(V_SPACE_USED, new TUintValue(READ_ONLY_VALUE | UINT_UNIT_VALUE));
    m->Add(V_INODE_USED, new TUintValue(READ_ONLY_VALUE | UINT_UNIT_VALUE));

    m->Add(V_SPACE_AVAILABLE, new TUintValue(READ_ONLY_VALUE | UINT_UNIT_VALUE));
    m->Add(V_INODE_AVAILABLE, new TUintValue(READ_ONLY_VALUE | UINT_UNIT_VALUE));
}

TError TVolumeHolder::Create(std::shared_ptr<TVolume> &volume) {
    uint16_t id;

    TError error = IdMap.Get(id);
    if (error)
        return error;
    auto node = Storage->GetNode(id);
    auto config = std::make_shared<TValueMap>(node);
    RegisterVolumeProperties(config);
    error = config->Set<int>(V_ID, id);
    if (error) {
        config->Remove();
        IdMap.Put(id);
        return error;
    }
    volume = std::make_shared<TVolume>(shared_from_this(), config);
    return TError::Success();
}

TError TVolumeHolder::RestoreFromStorage(std::shared_ptr<TContainerHolder> Cholder) {
    std::vector<std::shared_ptr<TKeyValueNode>> list;

    TPath volumes = config().volumes().volume_dir();
    if (!volumes.Exists() || volumes.GetType() != EFileType::Directory) {
        TFolder dir(config().volumes().volume_dir());
        (void)dir.Remove(true);
        TError error = dir.Create(0755, true);
        if (error)
            return error;
    }

    TError error = Storage->ListNodes(list);
    if (error)
        return error;

    for (auto &node : list) {
        L_ACT() << "Restore volume " << node->GetName() << std::endl;

        auto config = std::make_shared<TValueMap>(node);
        RegisterVolumeProperties(config);
        error = config->Restore();
        if (error || !config->HasValue(V_ID) ||
                IdMap.GetAt(config->Get<int>(V_ID))) {
            L_WRN() << "Corrupted volume config " << node << " removed: " << error << std::endl;
            (void)config->Remove();
            continue;
        }

        auto volume = std::make_shared<TVolume>(shared_from_this(), config);
        error = volume->Restore();
        if (error) {
            L_WRN() << "Corrupted volume " << node << " removed: " << error << std::endl;
            (void)volume->Destroy();
            continue;
        }

        error = Register(volume);
        if (error) {
            L_WRN() << "Cannot register volume " << node << " removed: " << error << std::endl;
            (void)volume->Destroy();
            continue;
        }

        for (auto name: volume->GetContainers()) {
            std::shared_ptr<TContainer> container;
            if (Cholder->Get(name, container))
                volume->UnlinkContainer(name);
            else
                container->LinkVolume(volume);
        }

        L() << "Volume " << volume->GetPath() << " restored" << std::endl;
    }

    L_ACT() << "Remove stale volumes..." << std::endl;
    RemoveIf(config().volumes().volume_dir(),
             EFileType::Directory,
             [&](const std::string &name, const TPath &path) {
                bool used = false;
                for (auto v : Volumes)
                    if (std::to_string(v.second->GetId()) == name)
                        used = true;
                return !used;
             });

    return TError::Success();
}

void TVolumeHolder::Destroy() {
    while (Volumes.begin() != Volumes.end()) {
        auto name = Volumes.begin()->first;
        auto volume = Volumes.begin()->second;
        TError error = volume->Destroy();
        if (error)
            L_ERR() << "Can't destroy volume " << name << ": " << error << std::endl;
    }
}

TError TVolumeHolder::Register(std::shared_ptr<TVolume> volume) {
    if (Volumes.find(volume->GetPath()) == Volumes.end()) {
        Volumes[volume->GetPath()] = volume;
        return TError::Success();
    }

    return TError(EError::VolumeAlreadyExists, "Volume already exists");
}

void TVolumeHolder::Unregister(std::shared_ptr<TVolume> volume) {
    Volumes.erase(volume->GetPath());
    IdMap.Put(volume->GetId());
}

std::shared_ptr<TVolume> TVolumeHolder::Find(const TPath &path) {
    auto v = Volumes.find(path);
    if (v != Volumes.end())
        return v->second;
    else
        return nullptr;
}

std::vector<TPath> TVolumeHolder::ListPaths() const {
    std::vector<TPath> ret;

    for (auto v : Volumes)
        ret.push_back(v.first);

    return ret;
}
