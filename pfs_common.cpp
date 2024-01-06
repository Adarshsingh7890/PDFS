#include "pfs_common.hpp"


std::string ipfile = "pfs_list.txt";

time_t getCurTime() {
    time_t currentTime = time(nullptr); // in Seconds
    return currentTime;
}

void setpfslist(string x) {
    ipfile = x;
}

int isMetaServer(std::string hostnameorip) {
    std::ifstream ipFile(ipfile);
    std::string line;
    std::getline(ipFile, line);

    if (line.find(hostnameorip) == 0) return 1;

    ipFile.close();

    return 0;
}

std::string getMetaServerAddress() {
    std::ifstream ipFile(ipfile);
    std::string line;
    std::getline(ipFile, line);
    ipFile.close();

    return line;
}

int isFileServer(std::string hostnameorip) {
    std::ifstream ipFile(ipfile);
    std::string line;
    while (std::getline(ipFile, line)) {
        if (line.find(hostnameorip) == 0) return 1;
    }

    ipFile.close();

    return 0;
}

map<int, string> getFileServers() {
    map<int, string> m;
    std::ifstream ipFile(ipfile);
    std::string line;
    int c = 0;
    while (std::getline(ipFile, line)) {
        if (c >= 1) {
            m[c - 1] = line;
        }
        c++;
    }

    ipFile.close();

    return m;
}

std::string getMyPort(std::string hostnameorip) {
    std::ifstream ipFile(ipfile);
    std::string line;
    while (std::getline(ipFile, line)) {
        if (line.find(hostnameorip) == 0) {
            return line;
        }
    }

    ipFile.close();

    return "";
}

// Get the current node's hostname
std::string getMyHostname()
{
    char hostname[255] = {0};
    gethostname(hostname, sizeof(hostname));

    std::string ret(hostname);
    return ret;
}

// Get the current node's IP address
std::string getMyIP()
{
    struct hostent *host_entry = gethostbyname(getMyHostname().c_str());
    char *temp = inet_ntoa(*((struct in_addr *)host_entry->h_addr_list[0]));

    std::string ret(temp);
    return ret;
}


vector<struct tokeninterval> missingTokenintervals(vector<struct tokeninterval> list, struct tokeninterval required) {
    //assume, list is sorted.
    vector<struct tokeninterval> missing;

    int n = list.size();
    int index = 0;

    // Iterate through the sorted intervals
    while (index < n && list[index].endoffset < required.startoffset) {
        index++;
    }

    // Find and add missing intervals within the given interval
    while (index < n && list[index].startoffset <= required.endoffset) {
        if (required.startoffset < list[index].startoffset) {
            struct tokeninterval mistoken;
            mistoken.startoffset = required.startoffset;
            mistoken.endoffset = list[index].startoffset - 1;
            missing.push_back(mistoken);
        }
        required.startoffset = std::max(required.startoffset, list[index].endoffset + 1);
        index++;
    }

    // Add the remaining interval if any
    if (required.startoffset <= required.endoffset) {
        missing.push_back(required);
    }

    return missing;
}

bool conflictInterval(struct tokeninterval m, struct tokeninterval n) {
    if ((m.startoffset > n.endoffset) || (n.startoffset > m.endoffset)) return false;
    return true;
}

bool conflict(struct heldorrequestedtokenbyclient p, struct heldorrequestedtokenbyclient q) {
    if (p.fd != q.fd) return false;

    if (p.clientid == q.clientid) return false;

    if (p.type == 1 && q.type == 1) return false;

    for (int i = 0; i < (int)p.tokens.size(); i++) {
        for (int j = 0; j < (int)q.tokens.size(); j++) {
            if (conflictInterval(p.tokens[i], q.tokens[j])) return true;
        }
    }

    return false;
}

//conflict token removed helper
vector<struct tokeninterval> excludetokeninterval(struct tokeninterval original, struct tokeninterval exclude) {
    std::vector<struct tokeninterval> result;

    // Check if exclude is entirely before or after the original interval
    if (exclude.endoffset <= original.startoffset || exclude.startoffset >= original.endoffset) {
        result.push_back(original);
        return result;
    }

    // Check if exclude overlaps with the left side of the original interval
    if (exclude.startoffset > original.startoffset) {
        struct tokeninterval t; 
        t.startoffset = original.startoffset;
        t.endoffset = exclude.startoffset - 1;
        result.push_back(t);
    }

    // Check if exclude overlaps with the right side of the original interval
    if (exclude.endoffset < original.endoffset) {
        struct tokeninterval t; 
        t.startoffset = exclude.endoffset + 1;
        t.endoffset = original.endoffset;
        result.push_back(t);
    }

    return result;
}


vector<struct tokeninterval> excludetokenintervals(vector<struct tokeninterval> originals, struct tokeninterval exclude) {
    std::vector<struct tokeninterval> result;
    for (int i = 0; i <(int) originals.size(); i++) {
        vector <struct tokeninterval> x = excludetokeninterval(originals[i], exclude);
        result.insert(result.end(), x.begin(), x.end());
    }

    
    return result;
}


struct heldorrequestedtokenbyclient exclude(struct heldorrequestedtokenbyclient origin, struct heldorrequestedtokenbyclient exc) {
    struct heldorrequestedtokenbyclient result;
    result = origin;
    vector<struct tokeninterval> originaltokens = origin.tokens;

    for (int i = 0; i < (int)exc.tokens.size(); i++) {
        struct tokeninterval exclude = exc.tokens[i];
        originaltokens = excludetokenintervals(originaltokens, exclude);
    }

    result.tokens = originaltokens;

    return result;
}

void printtokenlist(vector<struct tokeninterval> v) {
    for (int i = 0; i <(int) v.size(); i++) {
        v[i].print();
    }
}

void printtokenlistMeta(vector<struct heldorrequestedtokenbyclient> v) {
    for (int i = 0; i <(int) v.size(); i++) {
        v[i].print();
    }
}
