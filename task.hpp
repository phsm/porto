#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <memory>

#include "util/namespace.hpp"
#include "util/path.hpp"
#include "util/netlink.hpp"
#include "util/cred.hpp"

extern "C" {
#include <sys/resource.h>
}

class TTask;
class TContainerEnv;
class TFolder;
class TCgroup;
class TSubsystem;

struct TExitStatus {
    // Task was not started due to the following error
    int Error;
    // Task exited with given status
    int Status;
};

struct TBindMap {
    TPath Source;
    TPath Dest;
    bool Rdonly;
};

struct THostNetCfg {
    std::string Dev;
};

struct TMacVlanNetCfg {
    std::string Master;
    std::string Name;
    std::string Type;
    std::string Hw;
    int Mtu;
};

struct TIpMap {
    TNlAddr Addr;
    int Prefix;
};

struct TVethNetCfg {
    std::string Bridge;
    std::string Name;
    std::string Hw;
    std::string Peer;
    int Mtu;
};

struct TNetCfg {
    bool Share;
    std::vector<THostNetCfg> Host;
    std::vector<TMacVlanNetCfg> MacVlan;
    std::vector<TVethNetCfg> Veth;
};

class TTaskEnv : public TNonCopyable {
    friend TTask;
    TCred Cred;

public:
    TTaskEnv() {}
    std::string Command;
    TPath Cwd;
    bool CreateCwd;
    TPath Root;
    bool RootRdOnly;
    std::string User;
    std::string Group;
    std::vector<std::string> Environ;
    bool Isolate = false;
    TPath StdinPath;
    TPath StdoutPath;
    bool RemoveStdout;
    TPath StderrPath;
    bool RemoveStderr;
    TNamespaceSnapshot Ns;
    std::map<int,struct rlimit> Rlimit;
    std::string Hostname;
    bool BindDns;
    std::vector<TBindMap> BindMap;
    TNetCfg NetCfg;
    TPath Loop;
    int LoopDev;
    uint64_t Caps;
    TNlAddr DefaultGw;
    std::map<std::string, TIpMap> IpMap;
    bool NewMountNs;

    TError Prepare(const TCred &cred);
    const char** GetEnvp() const;
};

class TTask: public TNonCopyable {
    int Rfd, Wfd;
    int WaitParentRfd, WaitParentWfd;
    std::shared_ptr<TTaskEnv> Env;
    std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> LeafCgroups;

    enum ETaskState { Stopped, Started } State;
    int ExitStatus;

    pid_t Pid;
    std::shared_ptr<TFolder> Cwd;

    void ReportPid(int pid) const;

    TError RotateFile(const TPath &path) const;
    TError CreateCwd();
    TError CreateNode(const TPath &path, unsigned int mode, unsigned int dev);
    TError ChildOpenStdFile(const TPath &path, int expected);
    TError ChildReopenStdio();
    TError ChildApplyCapabilities();
    TError ChildDropPriveleges();
    TError ChildExec();
    TError ChildBindDns();
    TError ChildBindDirectores();
    TError ChildRestrictProc(bool restrictProcSys);
    TError ChildMountDev();
    TError ChildMountRun();
    TError ChildIsolateFs();
    TError EnableNet();
    TError IsolateNet(int childPid);
    bool IsValid();

public:
    TTask(std::shared_ptr<TTaskEnv> env,
          const std::map<std::shared_ptr<TSubsystem>, std::shared_ptr<TCgroup>> &leafCgroups) : Env(env), LeafCgroups(leafCgroups) {};
    TTask(pid_t pid) : Pid(pid) {};
    ~TTask();

    TError Start();
    int GetPid() const;
    bool IsRunning() const;
    int GetExitStatus() const;
    TError Kill(int signal) const;
    void DeliverExitStatus(int status);

    std::string GetStdout(size_t limit) const;
    std::string GetStderr(size_t limit) const;
    static void RemoveStdioFile(const TPath &path);
    void RemoveStdio() const;

    TError ChildApplyLimits();
    TError ChildSetHostname();
    TError ChildPrepareLoop();
    TError ChildRemountSlave();
    TError ChildCallback();
    TError Restore(int pid_,
                   const std::string &stdinPath,
                   const std::string &stdoutPath,
                   const std::string &stderrPath);
    TError FixCgroups() const;
    TError Rotate() const;
    void Abort(const TError &error) const;

    TError GetPPid(pid_t &ppid) const;
};

TError TaskGetLastCap();
