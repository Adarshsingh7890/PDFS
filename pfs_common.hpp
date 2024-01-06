#pragma once

#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <sys/types.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <string>
#include <iostream>
#include <fstream>
#include <vector>
#include <bits/stdc++.h>
#include <semaphore.h>
#include <bits/stdc++.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <map>
#include <errno.h>
#include <memory>
#include <semaphore.h>
#include <unordered_map>
#include "pfs_config.hpp"

#define ll long long int
#define CACHE_ENABLED 1

using namespace std;


struct fileopeninfo {
    int fd;
    int mode;
};


struct tokeninterval {
    ll startoffset;
    ll endoffset;
    bool operator<(const tokeninterval& other) const {
        return (startoffset < other.startoffset) || ((startoffset == other.startoffset) && (endoffset < other.endoffset));
    }

    void print() {
        cout<<startoffset << " " << endoffset <<endl;
    }
};

struct CacheBlock {
    int fd;
    ll blocknum;

    bool operator<(const CacheBlock& other) const {
        return (fd < other.fd) || ((fd == other.fd) && (blocknum < other.blocknum));
    }

};

struct heldorrequestedtokenbyclient {
    int fd;
    int clientid;
    vector<struct tokeninterval> tokens;
    int type;

    void print() {
        cout << "FD: " << fd << " " << "clientid:" << clientid << "TYPE:" << type <<endl;
        for (int i = 0; i < (int)tokens.size(); i++) tokens[i].print(); 
    }
};

struct requestedtokenbyclient {
    struct heldorrequestedtokenbyclient r;
    mutex mlock;
};

struct pfs_filerecipe
{
    int stripe_width;
    vector<int> servers;
    map<ll, int> blockToServerid;

    int getSid(ll blocknum) {
        ll x = (blocknum % ((ll) STRIPE_BLOCKS * stripe_width));
        int xx = (x / (int) STRIPE_BLOCKS);
        return servers[xx];
    }

};

struct pfs_metadata
{
    // Given metadata
    char filename[256];
    uint64_t file_size;
    time_t ctime;
    time_t mtime;
    struct pfs_filerecipe recipe;

    // Additional metadata
    int fd;
    int accessed; //pfs_open increase this, pfs-close decreses this

};

struct pfs_execstat
{
    long num_read_hits;
    long num_write_hits;
    long num_evictions;
    long num_writebacks;
    long num_invalidations;
    long num_close_writebacks;
    long num_close_evictions;
};

std::string getMyHostname();
std::string getMyIP();
std::string getMyPort(std::string hostnameorip);
int isMetaServer(std::string hostnameorip);
int isFileServer(std::string hostnameorip);
string getFileServer(int serverid);

std::string getMetaServerAddress();
time_t getCurTime();
vector<struct tokeninterval> missingTokenintervals(vector<struct tokeninterval> list, struct tokeninterval required);

bool conflict(struct heldorrequestedtokenbyclient p, struct heldorrequestedtokenbyclient q);
struct heldorrequestedtokenbyclient exclude(struct heldorrequestedtokenbyclient p, struct heldorrequestedtokenbyclient q);
vector<struct tokeninterval> excludetokeninterval(struct tokeninterval original, struct tokeninterval exclude);
vector<struct tokeninterval> excludetokenintervals(vector<struct tokeninterval> originals, struct tokeninterval exclude);
void printtokenlist(vector<struct tokeninterval> v);
void printtokenlistMeta(vector<struct heldorrequestedtokenbyclient> v);
map<int, string> getFileServers();
void setpfslist(string x);