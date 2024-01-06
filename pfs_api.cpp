#include "pfs_config.hpp"
#include "pfs_api.hpp"
#include "pfs_common.hpp"
#include "pfs_client.pb.h"
#include "pfs_client.grpc.pb.h"
#include <grpcpp/grpcpp.h>
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using google::protobuf::Empty;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;

void RunServer();
map<int, string> fileservers;
map <string, fileopeninfo> fileinfos;
map<int, string> fdToFilenameMap;
map<int, vector<struct tokeninterval> > fdToReadTokensList;
map<int, vector<struct tokeninterval> > fdToWriteTokensList;

mutex tokenconsistency;
bool revocation = 1;

int client_id = -1;
int clientAsServer = 0; 

int getmode(int fd) {
    int mode = -1;
    if (fdToFilenameMap.find(fd) != fdToFilenameMap.end()) {
        string fname = fdToFilenameMap[fd];
        return fileinfos[fname].mode;
    }

    return mode;
}

class FileClient {
public:
    FileClient(std::shared_ptr<Channel> channel) : stub_(FileService::NewStub(channel)) {}

    int isConnected() {
        ClientContext context;
        Empty response;
        Dummy request;

        Status status = stub_->isConnected(&context, request, &response);

        if (status.ok()) {
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int deleteFd(int fd) {
        ClientContext context;
        Empty response;
        File request;
        request.set_fd(fd);

        Status status = stub_->deleteFd(&context, request, &response);

        if (status.ok()) {
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    string readData(int fd, ll blocknum) {
        ClientContext context;
        Blockinfo request; request.set_fd(fd); request.set_blocknum(blocknum);
        Data response;
        
        Status status = stub_->readData(&context, request, &response);

        if (status.ok()) {
          return response.data();
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return "";
        }
    }

    int writeDataSpecific(int fd, ll blocknum, string out, ll offset, ll size) {
        ClientContext context;
        DataSpecific request; 
        request.set_fd(fd); request.set_blocknum(blocknum);
        request.set_data(out);
        request.set_startoffset(offset);
        request.set_size(size);
        Empty response;
        
        Status status = stub_->writeDataSpecific(&context, request, &response);

        if (status.ok()) {
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int writeData(struct CacheBlock cb, char * out) {
        ClientContext context;
        Data request; 
        request.set_fd(cb.fd); request.set_blocknum(cb.blocknum);
        request.set_data(string(out));
        Empty response;
        
        Status status = stub_->writeData(&context, request, &response);

        if (status.ok()) {
          return 0;
        } else {
          ////std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }
    

private:
    std::unique_ptr<FileService::Stub> stub_;
};

FileClient* fileclient[NUM_FILE_SERVERS];


struct Cache {
    char* blocks[CLIENT_CACHE_BLOCKS];
    map<struct CacheBlock, int> blockToPosMap;
    map<int, struct CacheBlock> posToBlockMap;
    ll timestamp[CLIENT_CACHE_BLOCKS];
    ll timestampCnt = 0;
    int serverid[CLIENT_CACHE_BLOCKS];
    bool dirty[CLIENT_CACHE_BLOCKS]; 

    mutex mtx;
    struct pfs_execstat *exestat;
    struct CacheBlock* imp;

    Cache() {
        exestat = new struct pfs_execstat();
        exestat->num_read_hits = 0;
        exestat->num_write_hits = 0;
        exestat->num_evictions = 0;
        exestat->num_writebacks = 0;
        exestat->num_invalidations = 0;
        exestat->num_close_writebacks = 0;
        exestat->num_close_evictions = 0;

        for (int i = 0; i < CLIENT_CACHE_BLOCKS; i++) {
            timestamp[i] = -1; //empty
            blocks[i] = NULL;
            dirty[i] = 0;
            serverid[i] = -1;
            imp = NULL;
        }
    }

    bool isImp(int fd, ll blocknum) {
        if (imp != NULL && imp->fd ==fd && imp->blocknum == blocknum) return true;
        return false;
    }

    char * getBlockFromCache(int fd, ll blockNumber) {
        struct CacheBlock cb;
        cb.fd = fd;
        cb.blocknum = blockNumber;

        // found
        if (blockToPosMap.find(cb) != blockToPosMap.end()) {
            int pos = blockToPosMap[cb];
            //put recent timestamp
            timestampCnt++;
            timestamp[pos] = timestampCnt;

            return blocks[pos];
        }

        return NULL; // not found
    }

    int findBlockposFromCache(int fd, ll blockNumber) {
        struct CacheBlock cb;
        cb.fd = fd;
        cb.blocknum = blockNumber;

        // found
        if (blockToPosMap.find(cb) != blockToPosMap.end()) {
            int pos = blockToPosMap[cb];
            return pos;
        }

        return -1;
    }

    int getEmptyOrLRUPos() {
        ll mn = 10000000000000LL;
        int pos;

        for (int i = 0; i < CLIENT_CACHE_BLOCKS; i++) {
            if (timestamp[i] < mn) {
                mn = timestamp[i];
                pos = i;
            }
        }

        return pos;
    }

    void setDirty(int fd, ll blockNumber) {
        struct CacheBlock cb;
        cb.fd = fd;
        cb.blocknum = blockNumber;

        // found
        if (blockToPosMap.find(cb) != blockToPosMap.end()) {
            int pos = blockToPosMap[cb];
            dirty[pos] = 1;
        }
    }

    
    void evictOrInvalidateBlock(int pos, bool evict, bool onclose) {
        //cout<<"I am evicted"<<endl;
        struct CacheBlock cb = posToBlockMap[pos];
        if (dirty[pos]) { //if dirty, needs writeback to FS
            struct CacheBlock cb = posToBlockMap[pos];
            fileclient[serverid[pos]]->writeData(cb, blocks[pos]);
            exestat->num_writebacks++;
            //cout<< "WB" <<exestat->num_writebacks<<endl;


            if (onclose) {
                exestat->num_close_writebacks++;
            }
        }

        blocks[pos] = NULL;
        timestamp[pos] = -1;
        dirty[pos] = 0;
        blockToPosMap.erase(cb);
        posToBlockMap.erase(pos);
        serverid[pos] = -1;

        if (evict) {
            exestat->num_evictions++;
            if (onclose) exestat->num_close_evictions++;
        } else {
            exestat->num_invalidations++;
        }
    }

    void invalidatecb(int fd, ll blocknum) {
        int pos = findBlockposFromCache(fd, blocknum);
        if (pos != -1) {
            evictOrInvalidateBlock(pos, false, false);
        }
    }

    void placeBlockIncache(int fd, ll blockNumber, char* blockholder, bool dirt, int sid) {
        struct CacheBlock cb;
        cb.fd = fd;
        cb.blocknum = blockNumber;

        int pos = getEmptyOrLRUPos();
        if (timestamp[pos] != -1) { //eviction needed
            evictOrInvalidateBlock(pos, true, false);
        } else {
            //empty block
            //nothing needds to do

        }


        blocks[pos] = blockholder;
        timestampCnt++;
        timestamp[pos] = timestampCnt;
        dirty[pos] = dirt;
        blockToPosMap[cb] = pos;
        posToBlockMap[pos] = cb;
        serverid[pos] = sid;
    }

    void evictBlocksonClose(int fd) {
        for (int i = 0; i < CLIENT_CACHE_BLOCKS; i++) {
            if (timestamp[i] != -1) {
                struct CacheBlock cb = posToBlockMap[i];
                if (cb.fd == fd) {
                    evictOrInvalidateBlock(i, true, true);
                }
            }
        }
    }

};

struct Cache* mycache = NULL;


class Client {
public:
    Client(std::shared_ptr<Channel> channel) : stub_(MetaService::NewStub(channel)) {
    }

    ll getfilesize(int fd) {
        ClientContext context;
        File request, response;
        request.set_fd(fd);
        
        Status status = stub_->getfilesize(&context, request, &response);

        if (status.ok()) {
          return response.filesize();
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int setfilesize(int fd, ll filesize) {
        ClientContext context;
        File request, response;
        request.set_fd(fd);
        request.set_filesize(filesize);
        
        Status status = stub_->setfilesize(&context, request, &response);

        if (status.ok()) {
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int getClientId(ClientInfo request) {
        ClientContext context;
        ClientId response;
        
        Status status = stub_->getClientId(&context, request, &response);

        if (status.ok()) {
          return response.id();
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int startClientService(ClientInfo request) {
        ClientContext context;
        Empty response;
        
        Status status = stub_->startClientService(&context, request, &response);

        if (status.ok()) {
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int canCacheBlock(int fd, ll blockNumber, int full, int type) {
        ClientContext context;
        CacheRequest request;
        Blockinfo* blockinfo = request.mutable_block();
        blockinfo->set_fd(fd); 
        blockinfo->set_blocknum(blockNumber);
        
        request.set_type(type);
        request.set_full(full);
        request.set_clientid(client_id);
        ResponseCode response;
        Status status = stub_->canCacheBlock(&context, request, &response);

        if (status.ok()) {
          return response.val();
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int createFile(FileCreateRequest request) {
        ClientContext context;
        ResponseCode response;
        Status status = stub_->createFile(&context, request, &response);

        if (status.ok()) {
          return response.val();
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int deleteFile(FileDeleteRequest request) {
        ClientContext context;
        Sidlist response;
        Status status = stub_->deleteFile(&context, request, &response);

        if (status.ok()) {
            if (response.sids().size() > 0) {
                for (int i = 0; i < (int)response.sids().size(); i++) {
                    fileclient[response.sids(i)]->deleteFd(response.fd());
                }
            }

            return response.res();
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }


    int openFile(FileOpenRequest request) {
        ClientContext context;
        ResponseCode response;
        Status status = stub_->openFile(&context, request, &response);

        if (status.ok()) {
          return response.val();
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int closefd(int fd) {
        ClientContext context;
        CloseRequest request;
        request.set_fd(fd);
        request.set_clientid(client_id);
        Empty response;
        Status status = stub_->closefd(&context, request, &response);

        if (status.ok()) {
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int fstat(int fd, struct pfs_metadata* meta_data) {
        ClientContext context;
        ResponseCode request;
        request.set_val(fd);
        Metadata response;
        Status status = stub_->fstat(&context, request, &response);

        if (status.ok()) {
            int resp = response.res();
            if (resp == 0) {
                memcpy(meta_data->filename, response.fname().c_str(), response.fname().size());
                meta_data->file_size = response.filesize();
                meta_data->ctime = response.ctime();
                meta_data->mtime = response.mtime();
            }
           return resp;
        } else {
           //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
           return -1;
        }
    }



    int requestToken(Token request) {
        ClientContext context;
        ResponseCode response;
        Status status = stub_->requestToken(&context, request, &response);

        if (status.ok()) {
          return response.val();
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int getServerInfoForBlock(int fd, ll blocknum) {
        ClientContext context;
        
        Blockinfo request;
        request.set_fd(fd); request.set_blocknum(blocknum);

        ResponseCode response;
        Status status = stub_->getServerInfoForBlock(&context, request, &response);

        if (status.ok()) {
          return response.val();
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }
    

private:
    std::unique_ptr<MetaService::Stub> stub_;
};

std::shared_ptr<Channel> channel;
Client* client = NULL;



/*
Init the PFS client. Return a positive value client id allocated by the metadata server. 
Return -1 on error (e.g., can't communicate with metadata server, file servers (total of NUM_FILE_SERVERS) are not online yet, etc.).
The metadata and file servers' ip address and port can be found in pfs_list.txt. 
You may need to allocate a struct pfs_execstat internally. You may need to initialize the client cache.
 */
int pfs_initialize()
{
    setpfslist("../pfs_list.txt");
    
    ClientInfo info;
    info.set_client_ip(getMyIP());
    info.set_client_name(getMyHostname());
    
    if (client == NULL) {
        channel = grpc::CreateChannel(getMetaServerAddress(), grpc::InsecureChannelCredentials());
        client = new Client(channel);

    }

    fileservers = getFileServers();

    for (int i = 0; i < NUM_FILE_SERVERS; i++) {
        if (fileservers.find(i) == fileservers.end()) {
            cout<< i << " File server missing" << endl;
            return -1;
        } else {
            fileclient[i] = new FileClient(grpc::CreateChannel(fileservers[i], grpc::InsecureChannelCredentials()));
            int ok = fileclient[i]->isConnected();
            if (ok == -1) return -1;
            //is fS alive? else -1;
        }
    }
    
    

    client_id = client->getClientId(info);
    if (client_id == -1) return -1;

    thread serverthread(RunServer);
    serverthread.detach();

    info.set_clientid(client_id);

    while (!clientAsServer);
    //int ok = 
    client->startClientService(info);
    //cout<<"MS started service for clients:" <<ok<<endl;
    
    mycache = new struct Cache(); //cache initialized

    return client_id;
}


/*Creates a file with the name filename. The file will be in striped into stripe_width number of servers.
    Return 0 on success. Return -1 on error (e.g., duplicate filename, stripe_width larger than
    NUM_FILE_SERVERS, etc). This will also create the metadata on the metadata server. Users can
    access the metadata via pfs_fstat()
*/
int pfs_create(const char *filename, int stripe_width)
{
    if (stripe_width > NUM_FILE_SERVERS) {
        return -1;
    }

    FileCreateRequest request;
    request.set_filename(std::string(filename));
    request.set_stripe_width(stripe_width);

    if (client != NULL) {
        int resp = client->createFile(request);
        return resp;
    }

    return -1;
}
/*
Opens a file with name filename. The mode will be either 1 (read) or 2 (read/write). 
Return a positive value file descriptor. Return -1 on error (e.g., non-existing file, duplicate open, etc).
*/
int pfs_open(const char *filename, int mode)
{
    auto it = fileinfos.find(std::string(filename));
    //already opened.
    if (it != fileinfos.end()) {
        return -1;
    }

    string fname = std::string(filename);
    FileOpenRequest request;
    request.set_filename(fname);
    request.set_mode(mode); //dont need to send actually....

    if (client != NULL) {
        int fd = client->openFile(request);
        if (fd != -1) { //file exists
            struct fileopeninfo info;
            info.fd = fd;
            info.mode = mode;

            fileinfos[fname] = info; // insert this in openfile list
            fdToFilenameMap[fd] = fname;

        }

        return fd;
    }

    return -1;
}

int getReadToken(int fd, ll startoffset, ll endoffset) {
    //cout<<"enter"<<endl;
    //tokenconsistency.lock();

    struct tokeninterval required;
    required.startoffset = startoffset;
    required.endoffset = endoffset;

    vector<struct tokeninterval> missing;

    vector<struct tokeninterval> rdwrall;
    auto it = fdToReadTokensList.find(fd);
    if (it != fdToReadTokensList.end()) {
        rdwrall.insert(rdwrall.end(), fdToReadTokensList[fd].begin(), fdToReadTokensList[fd].end());
    } else {
        vector<struct tokeninterval> v;
        fdToReadTokensList[fd] = v;
    }

    it = fdToWriteTokensList.find(fd);
    if (it != fdToWriteTokensList.end()) {
        rdwrall.insert(rdwrall.end(), fdToWriteTokensList[fd].begin(), fdToWriteTokensList[fd].end());
    } else {
        vector<struct tokeninterval> v;
        fdToWriteTokensList[fd] = v;
    }

    sort(rdwrall.begin(), rdwrall.end());
    missing = missingTokenintervals(rdwrall, required);

    if (missing.size() == 0) {
        //no token request needed
        //cout<< "local" << endl;

        //after read/write done.
        //tokenconsistency.unlock();

    } else {
        // need tokens from metaserver. if MS cant give us tokens, it will signal that tokenconsistency needs to be unlocked
        //as it is blocked for someitme in the metaserver.
        //tokenconsistency.unlock(); // because it may take time.

        Token t;
        t.set_clientid(client_id);
        t.set_fd(fd);
        t.set_type(1); // read
        /*for (int i = 0; i < (int)missing.size(); i++) {
            Tokeninterval* ti = t.add_tokenintervals();
            ti->set_startoffset(missing[i].startoffset);
            ti->set_endoffset(missing[i].endoffset);
        }*/
        
        Tokeninterval* ti = t.add_tokenintervals();
        ti->set_startoffset(required.startoffset);
        ti->set_endoffset(required.endoffset);

        //just unblock lock because I am blocked in RPC
        tokenconsistency.unlock();

        int res = client->requestToken(t);
        
        //cout << "successful"<<endl;
        if (res == 0) 
        {
            tokenconsistency.lock(); // pick the lock again after getting back from wait
            revocation = 1; //imp
        } else {
            //cout <<"error while getting bck from RPC write request toekn" <<endl;
        }

        //add acquired token to own tken list
        //fdToWriteTokensList[fd].insert(fdToWriteTokensList[fd].end(), missing.begin(), missing.end());
        fdToReadTokensList[fd].push_back(required);
    

        //after read/write done.
        //printtokenlist(fdToReadTokensList[fd]);
        //tokenconsistency.unlock();
    }

    //cout<<"end"<<endl;


    return 0;
}

int getWriteToken(int fd, ll startoffset, ll endoffset) {
    //cout<<"enter"<<endl;
    //--moved to actal read/write calltokenconsistency.lock();

    struct tokeninterval required;
    required.startoffset = startoffset;
    required.endoffset = endoffset;

    vector<struct tokeninterval> missing;
    
    auto it = fdToWriteTokensList.find(fd);
    if (it != fdToWriteTokensList.end()) {
        sort(fdToWriteTokensList[fd].begin(), fdToWriteTokensList[fd].end());
        missing = missingTokenintervals(fdToWriteTokensList[fd], required); 
    } else {
        vector<struct tokeninterval> v;
        fdToWriteTokensList[fd] = v;
        missing.push_back(required);
    }

    if (missing.size() == 0) {
        //no token request needed
        //cout<< "local" << endl;

        //after read/write done.
        //--moved to actal read/write calltokenconsistency.lock();

    } else {
        // need tokens from metaserver. if MS cant give us tokens, it will signal that tokenconsistency needs to be unlocked
        //as it is blocked for someitme in the metaserver.
        //tokenconsistency.unlock(); // because it may take time.

        Token t;
        t.set_clientid(client_id);
        t.set_fd(fd);
        t.set_type(2);
        /*for (int i = 0; i < (int)missing.size(); i++) {
            Tokeninterval* ti = t.add_tokenintervals();
            ti->set_startoffset(missing[i].startoffset);
            ti->set_endoffset(missing[i].endoffset);
        }*/
        
        Tokeninterval* ti = t.add_tokenintervals();
        ti->set_startoffset(required.startoffset);
        ti->set_endoffset(required.endoffset);

        //just unblock lock because I am blocked in RPC
        tokenconsistency.unlock();

        int res = client->requestToken(t); 
        
        //cout << "successful"<<endl;
        if (res == 0) 
        {
            tokenconsistency.lock(); // pick the lock again after getting back from wait
            revocation = 1;
        } else {
            //cout <<"error while getting bck from RPC write request toekn" <<endl;
        }

        //add acquired token to own tken list
        //fdToWriteTokensList[fd].insert(fdToWriteTokensList[fd].end(), missing.begin(), missing.end());
        fdToWriteTokensList[fd].push_back(required);
    

        //after read/write done.
        //printtokenlist(fdToWriteTokensList[fd]);
        //--moved to actal read/write call tokenconsistency.lock();
    }

    //cout<<"end"<<endl;


    return 0;
}

/*
Reads num_bytes bytes from the file fd, reading starts from offset~. 
Return the actual number of read bytes (may be smaller than the requested, 
if the offset is close to end-of-file). (A return of zero indicates end-of-file.) 
Return -1 on error (e.g., offset is larger than the filesize, non-existing file descriptor, 
communication with metadata/file server failed, etc.)
*/
int pfs_read(int fd, void *buf, size_t num_bytes, off_t offset)
{
    //lock to guard token revocation request from Metaserver
    //getToken -can be waited fhere for sometime
        //--already acquired? check the missing token locally
        //asks metaserver for missing tokens
    //getBlocks - from fileservers
        //for each block that is not local - CanIcacheblocks(blocknum)
    //read/write locally or remotely

    //check file size first--whether this valid or not
    if (getmode(fd) == -1) return -1;

    ll filesize = client->getfilesize(fd);
    if (filesize < 0) return -1;

    if (offset < 0 || offset > filesize) {
        return -1;
    }

    int size =  (int)num_bytes;

    if (size == 0) return 0;

    ll endoffset = offset + (ll)size - 1; // 0-1023 = 0 + 1024 - 1 = 123;

    if (endoffset > (filesize - 1)) endoffset = filesize - 1;

    size = endoffset - offset + 1;

    if (size <= 0) return 0;





    //if pwrite succesful, update filesize and modification time (can before/after..thnk)
    tokenconsistency.lock(); // should I move this before acuire tokwn??
    ////
    //1. need to check whether this call itself is valid or not.//client != null, connection is ok

    //2.acquire token 
    getReadToken(fd, offset, endoffset); //can be  blocked for sometime here.

    //3. get the blocks
    ll curoffset = offset;
    int bufI = 0;
    while (curoffset <= endoffset) {
        //fetchBlock with curoffset
        ll blockNumber = curoffset / PFS_BLOCK_SIZE;
        ll blockstartoffset = blockNumber * PFS_BLOCK_SIZE;
        ll blockendoffset = ((blockNumber + 1) * PFS_BLOCK_SIZE) - 1;
        int full = (blockstartoffset >= offset) && (blockendoffset <= endoffset);
        char * blockholder = NULL;
        int canCache = 0;
        int sid;

        if (CACHE_ENABLED) {
            mycache->mtx.lock();
            blockholder = mycache->getBlockFromCache(fd, blockNumber);

            if (blockholder == NULL) //read-miss
            {
                struct CacheBlock* cb = new struct CacheBlock();
                cb->fd = fd;
                cb->blocknum = blockNumber;
                mycache->imp = cb; 
                mycache->mtx.unlock();
                canCache = client->canCacheBlock(fd, blockNumber, full, 1);
                mycache->mtx.lock();
                
            } else {
                mycache->exestat->num_read_hits++;
            }
        }

        if (blockholder == NULL) {
            sid = client->getServerInfoForBlock(fd, blockNumber);
            if (sid == -1) return -1;

            string blockdata = fileclient[sid]->readData(fd, blockNumber);
            blockholder = (char*)malloc(PFS_BLOCK_SIZE);
            for(int i = 0; i < (int)blockdata.size(); i++) blockholder[i] = blockdata[i]; //get current block data
        }
        

        ll internalcuroffset = curoffset - blockstartoffset;
        ll sz;
        if (endoffset >= blockendoffset) sz = blockendoffset - curoffset + 1;
        else {
            sz = endoffset - curoffset + 1;
        }

        //blockplaceholder-for cache
        memcpy((char *)buf + bufI, blockholder + internalcuroffset, sz);

        if (canCache) {
            mycache->placeBlockIncache(fd, blockNumber, blockholder, false, sid);
        }

        if (CACHE_ENABLED) {
            mycache->imp = NULL;
            mycache->mtx.unlock();
        }        


        bufI += sz;
        curoffset += sz;
    }


    
    tokenconsistency.unlock();

    return size;
}

/*
Writes num_bytes bytes to the file fd, writing starts from offset~. 
Return the number of written bytes. Return -1 on error.
pfs_write() will require offset to be: 0<=offset<=file size.
as the file size increases (due to write), you will distribute the blocks as above.
You can store this information in the file recipe. 
*/
int pfs_write(int fd, const void *buf, size_t num_bytes, off_t offset)
{
    //check file size first--whether this valid or not
    int mode = getmode(fd);
    if (mode != 2) return -1;


    ll filesize = client->getfilesize(fd);
    //cout<<"FSIZE: " << filesize<<endl;
    if (filesize < 0) return -1; //non-existent

    if (offset < 0 || offset > filesize) {
        return -1;
    }

    int size =  (int)num_bytes;
    if (size == 0) return 0;
    ll endoffset = offset + (ll)size - 1; // 0-1023 = 0 + 1024 - 1 = 123;

    //cout<<"W  " << offset << " " << endoffset << endl;
    //if pwrite succesful, update filesize and modification time (can before/after..thnk)
    tokenconsistency.lock(); // should I move this before acuire tokwn??
    ////
    //1. need to check whether this call itself is valid or not.//client != null, connection is ok

    //2.acquire token 
    getWriteToken(fd, offset, endoffset); //can be  blocked for sometime here.

    if (endoffset + 1 > filesize) filesize = endoffset + 1;
    client->setfilesize(fd, filesize); //mtime included
    //cout<<ok<<endl;
    //3. get the blocks
    ll curoffset = offset;
    int bufI = 0;
    while (curoffset <= endoffset) {
        //fetchBlock with curoffset
        ll blockNumber = curoffset / PFS_BLOCK_SIZE;
        ll blockstartoffset = blockNumber * PFS_BLOCK_SIZE;
        ll blockendoffset = ((blockNumber + 1) * PFS_BLOCK_SIZE) - 1;

        int full = (blockstartoffset >= offset) && (blockendoffset <= endoffset);
        char * blockholder = NULL;
        int canCache = 0;
        int sid;

        if (CACHE_ENABLED) {
            mycache->mtx.lock();
            blockholder = mycache->getBlockFromCache(fd, blockNumber);

            if (blockholder == NULL) //write-miss
            {
                struct CacheBlock* cb = new struct CacheBlock();
                cb->fd = fd;
                cb->blocknum = blockNumber;
                mycache->imp = cb; 
                mycache->mtx.unlock();
                canCache = client->canCacheBlock(fd, blockNumber, full, 2);
                mycache->mtx.lock();

            } else {
                mycache->exestat->num_write_hits++;
            }
        }

        if (blockholder == NULL) {
            sid = client->getServerInfoForBlock(fd, blockNumber);
            if (sid == -1) return -1;

            string blockdata = fileclient[sid]->readData(fd, blockNumber);
            blockholder = (char*)malloc(PFS_BLOCK_SIZE);
            for(int i = 0; i < (int)blockdata.size(); i++) blockholder[i] = blockdata[i]; //get current block data
        }


        ll internalcuroffset = curoffset - blockstartoffset;
        ll sz;
        if (endoffset >= blockendoffset) sz = blockendoffset - curoffset + 1;
        else {
            sz = endoffset - curoffset + 1;
        }

        if (CACHE_ENABLED != 1) {
            //cout<< "writing to specific offset" <<endl;
            char *internalmodification = (char*)malloc(sz);
            memcpy(internalmodification, (char *)buf + bufI, sz);
            
            //cout<<string(internalmodification) << "wrote to "<< blockNumber<<endl; 
            fileclient[sid]->writeDataSpecific(fd, blockNumber, string(internalmodification), internalcuroffset, sz); 
        } else {
            //blockplaceholder-for cache
            memcpy(blockholder + internalcuroffset, (char *)buf + bufI, sz); // modified
            
            if (canCache) {
                mycache->placeBlockIncache(fd, blockNumber, blockholder, true, sid);
            } else {
                mycache->setDirty(fd, blockNumber); //always for pwrite
            }
        }
        
        if (CACHE_ENABLED) {
            mycache->imp = NULL;
            mycache->mtx.unlock();
        } 

        bufI += sz;
        curoffset += sz;
    }


    //file size and last modfication time of fd--needs to be checked.

    ///
    tokenconsistency.unlock();

    return (int)num_bytes;
}
/*
Close a file with file descriptor fd. Return 0 on success. Return -1 on error (e.g., non-existing file
descriptor, etc.
*/
int pfs_close(int fd)
{
    //file open check
    if (fdToFilenameMap.find(fd) == fdToFilenameMap.end()) {
        return -1;
    } else {
        string fname = fdToFilenameMap[fd];
        fdToFilenameMap.erase(fd);
        fileinfos.erase(fname);
    }

    //fd related everything will be removed
    tokenconsistency.lock();
    if (fdToReadTokensList.find(fd) != fdToReadTokensList.end()) {
        fdToReadTokensList.erase(fd);
    }

    if (fdToWriteTokensList.find(fd) != fdToWriteTokensList.end()) {
        fdToWriteTokensList.erase(fd);
    }

    tokenconsistency.unlock();

    //evict caches for ths fd
    if (CACHE_ENABLED) {
        mycache->evictBlocksonClose(fd);
    }
 
    //inform server
    client->closefd(fd);

    return 0;
}
/*
Delete a file with the name filename. Return 0 on success. Return -1 on error
*/
int pfs_delete(const char *filename)
{
    FileDeleteRequest request;
    request.set_filename(std::string(filename));
    
    if (client != NULL) {
        int resp = client->deleteFile(request);
        return resp;
    }

    return -1;
}
/*
Write the metadata of file fd to the pointer meta_data. Return 0 on success. Return -1 on error.
*/
int pfs_fstat(int fd, struct pfs_metadata *meta_data)
{
    if (client != NULL) {
        int resp = client->fstat(fd, meta_data);
        return resp;
    }
    return -1;
}
/*
Write the statistics of the entire execution to the pointer execstat_data. Return 0 on success.
Return -1 on error. Note that this is a local operation (stat of only the calling client is returned)
*/
int pfs_execstat(struct pfs_execstat *execstat_data)
{
    if (mycache != NULL) {
        execstat_data->num_read_hits = mycache->exestat->num_read_hits;
        execstat_data->num_write_hits = mycache->exestat->num_write_hits;
        execstat_data->num_evictions = mycache->exestat->num_evictions;
        execstat_data->num_writebacks = mycache->exestat->num_writebacks;
        execstat_data->num_invalidations = mycache->exestat->num_invalidations;
        execstat_data->num_close_writebacks = mycache->exestat->num_close_writebacks;
        execstat_data->num_close_evictions = mycache->exestat->num_close_evictions;

        return 0;
    }
    return -1;
}

void processRevocation(const Token* t) {
    int fd = t->fd();
    int type = t->type();

    if (fdToWriteTokensList.find(fd) != fdToWriteTokensList.end()) {

        vector <struct tokeninterval> v = fdToWriteTokensList[fd];
        for (int i = 0; i < t->tokenintervals().size(); i++) {
            struct tokeninterval exc;
            exc.startoffset = t->tokenintervals(i).startoffset();
            exc.endoffset = t->tokenintervals(i).endoffset();
            v = excludetokenintervals(v, exc);
        }

        fdToWriteTokensList[fd] = v;
    }

    if (type == 2 && fdToReadTokensList.find(fd) != fdToReadTokensList.end()) {
        vector <struct tokeninterval> v = fdToReadTokensList[fd];
        for (int i = 0; i < t->tokenintervals().size(); i++) {
            struct tokeninterval exc;
            exc.startoffset = t->tokenintervals(i).startoffset();
            exc.endoffset = t->tokenintervals(i).endoffset();
            v = excludetokenintervals(v, exc);
        }

        fdToReadTokensList[fd] = v;

    }


    //invalidate caches
    if (CACHE_ENABLED) {
        for (int i = 0; i < t->tokenintervals().size(); i++) {
            ll startoffset = t->tokenintervals(i).startoffset();
            ll endoffset = t->tokenintervals(i).endoffset();

            ll startblock = startoffset / PFS_BLOCK_SIZE;
            ll endblock = endoffset / PFS_BLOCK_SIZE;

            for (ll j = startblock; j <= endblock; j++) mycache->invalidatecb(fd, j);
        }
    }
}

///client as a server
class ServerImpl final : public ClientService::Service {
public:
    Status isConnected(ServerContext* context, const ClientInfo* request, Empty* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;
        return Status::OK;
    }

    Status revokeTokens(ServerContext* context, const Token* request, Empty* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;
        while (1) {
            tokenconsistency.lock();
            if (!revocation) {
                tokenconsistency.unlock();
                for (int i = 0; i < 100; i++);
            } else {
                //cout<< "revocation request from" << request->clientid() << endl;
                processRevocation(request);
                //cout<< "revocation request completed from" << request->clientid() << endl;
                tokenconsistency.unlock();
                break;
            }
        }

        return Status::OK;
    }


    Status invalidateCacheBlock(ServerContext* context, const Blockinfo* request, Empty* response) override {
        //std::cout << "Received request for cache invalidation: " << std::endl;
        int fd = request->fd();
        ll blockNumber = request->blocknum();
        while (1) {
            mycache->mtx.lock();
            if (mycache->isImp(fd, blockNumber)) {
                tokenconsistency.unlock();
                for (int i = 0; i < 100; i++);
            } else {
                //cout<< "invalidation cache request from" << request->clientid() << endl;
                mycache->invalidatecb(fd, blockNumber);
                //cout<< "invalidation cache completed from" << request->clientid() << endl;
                mycache->mtx.unlock();
                break;
            }
        }

        return Status::OK;
    }

    /*Status setUnlock(ServerContext* context, const Empty* request, Empty* response) override {
        std::cout << "Received request: " << request->DebugString() << std::endl;
        tokenconsistency.unlock();
        return Status::OK;
    }*/

    Status stopRevocation(ServerContext* context, const File* request, Empty* response) override {
        //std::cout << "Received request for stopRevocation: " << std::endl;
        tokenconsistency.lock();
        revocation = 0;
        tokenconsistency.unlock();
        return Status::OK;
    }
};

void RunServer() {
    if (client_id == -1) {
        //cout<<"Client is not set" <<endl;
        return;
    }

    ServerImpl service;

    ServerBuilder builder;
    string serverAddress = getMyIP() + ":" + to_string(60000 + client_id);
    builder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "clinet listening on " << serverAddress << std::endl;
    clientAsServer = 1;

    server->Wait();
}