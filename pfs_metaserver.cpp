#include "pfs_metaserver.hpp"
#include <grpcpp/grpcpp.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using google::protobuf::Empty;
mutex mtx, fileMetadatasMtx;

int client_counter = 0;
int fd_counter = 0;
sem_t s;
sem_t p;
struct ClientData {
    ClientInfo info;
    ClientId id;
};

vector<ClientData> clients;
map <string, pfs_metadata> fileMetadatas;
map<int, string> fdToFilenameMap;



//cache DS
struct CacheInfo {
    mutex mtx;
    vector<int>cids;
    vector<int>ptypes;

};


map<struct CacheBlock, struct CacheInfo*> cachemap;
mutex cachemapconsistency; 


//token management DS
vector<struct heldorrequestedtokenbyclient> tokensAcquiredclients; //map[fd] = {{clientid: read/writetokens}, {clientid1: read/writetokens}..}
vector<struct requestedtokenbyclient*> tokensRequestedclients;
mutex tokenconsistency;

void acquireToken(struct heldorrequestedtokenbyclient h);

class Client {
public:
    Client(std::shared_ptr<Channel> channel) : stub_(ClientService::NewStub(channel)) {
    }

    int isConnected(ClientInfo request) {
        //cout<<1<< "---@@@@@@";
        ClientContext context;
        Empty response;
        
        Status status = stub_->isConnected(&context, request, &response);

        if (status.ok()) {
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int revokeTokens(Token t) {
        //        cout<<12<< "---@@@@@@";

        ClientContext context;
        Empty response;
        
        Status status = stub_->revokeTokens(&context, t, &response);

        if (status.ok()) {
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int setUnlock() {
                //cout<<3<< "---@@@@@@";

        ClientContext context;
        Empty response;
        Empty request;
        
        Status status = stub_->setUnlock(&context, request, &response);

        if (status.ok()) {
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int stopRevocation() {
                //cout<<4<< "---@@@@@@";

        ClientContext context;
        Empty response;
        File request;
        
        Status status = stub_->stopRevocation(&context, request, &response);

        if (status.ok()) {
          //cout<<"OKKKK" <<endl;
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    int invalidateCacheBlock(int fd, ll blocknum) {
                        //cout<<5<< "---@@@@@@";

        ClientContext context;
        Empty response;
        Blockinfo request;
        request.set_fd(fd);
        request.set_blocknum(blocknum);
        
        Status status = stub_->invalidateCacheBlock(&context, request, &response);

        if (status.ok()) {
          return 0;
        } else {
          //std::cerr << "RPC failed: " << status.error_code() << ": " << status.error_message() << std::endl;
          return -1;
        }
    }

    private:
    std::unique_ptr<ClientService::Stub> stub_;
};

Client* c[1000];

void removeTokens(int fd, int client_id) {
    vector<heldorrequestedtokenbyclient> v;
    for (int i = 0; i < (int) tokensAcquiredclients.size(); i++) {
        if (tokensAcquiredclients[i].fd != fd || tokensAcquiredclients[i].clientid != client_id) {
            if (tokensAcquiredclients[i].tokens.size() != 0) v.push_back(tokensAcquiredclients[i]);
        }
    }

    tokensAcquiredclients = v;
}

void removeClientFromCBMap(int fd, int client_id) {
    if (fd != -1) {
        cachemapconsistency.lock();
        for (auto it = cachemap.begin(); it != cachemap.end(); ++it) {
            struct CacheBlock cb = it->first;
            if (cb.fd == fd) {
                struct CacheInfo* cinfo = it->second;
                cinfo->mtx.lock();
                for (int i = 0; i < (int)cinfo->cids.size(); i++) {
                    if (cinfo->cids[i] == client_id) {
                        cinfo->cids.erase(cinfo->cids.begin() + i);
                        cinfo->ptypes.erase(cinfo->ptypes.begin() + i);
                    }
                }
                cinfo->mtx.unlock();
            }
        }

        cachemapconsistency.unlock();
    }
}

void pendingTokenRequestProcess() {
    while(1) {
        sem_wait(&s);

        tokenconsistency.lock();

        //cout<< "revocation for cpflicts tokens for pending request starts" <<endl;

        struct requestedtokenbyclient* rt = tokensRequestedclients[0];
        tokensRequestedclients.erase(tokensRequestedclients.begin());

        struct heldorrequestedtokenbyclient r = rt->r;
        vector<heldorrequestedtokenbyclient> excludedTokensAcquired;

        Token t;
        t.set_fd(r.fd);
        t.set_clientid(r.clientid);
        t.set_type(r.type);
        for (int i = 0; i < (int)r.tokens.size(); i++) {
            Tokeninterval* ti = t.add_tokenintervals();
            ti->set_startoffset(r.tokens[i].startoffset);
            ti->set_endoffset(r.tokens[i].endoffset);
        }

        


        set<int> conflictclients;

        for (int i = 0; i < (int)tokensAcquiredclients.size(); i++) {
            if (conflict(tokensAcquiredclients[i], r)) {
                int clientid = tokensAcquiredclients[i].clientid;
                if (conflictclients.find(clientid) == conflictclients.end()) {
                    c[clientid]->revokeTokens(t) ;//client side done here
                    conflictclients.insert(clientid);
                    //else cout<<"error"<<endl;                    
                }

                excludedTokensAcquired.push_back(exclude(tokensAcquiredclients[i], r));
            } else {
                if (tokensAcquiredclients[i].tokens.size() > 0) {
                    excludedTokensAcquired.push_back(tokensAcquiredclients[i]);
                }
                //need to check whether tokens.size == 0. if yes, do not need to add this. later will add this.[cleaning]
            }
        }

        tokensAcquiredclients = excludedTokensAcquired; //exclusion done in server side.
        
        //send client that u should not revoke tokens until your current operation finished.
        //int status = 
        c[r.clientid]->stopRevocation();


        //cache stuff
        if (CACHE_ENABLED) {
            //for (int )
        }

        //acquire all token reuqired for the client
        acquireToken(r);
        //just before coming out, unlock that requests thread
        rt->mlock.unlock();

        tokenconsistency.unlock();

        //cout<< "pending request succesully acquired its tokens" <<endl;

        //pipeline tokenconsistency locking--needed??
        //sem_wait(&p);
    }
}

int findClient(ClientInfo clientinfo) {
    for (int i = 0; i < (int)clients.size(); i++) {
        if(clients[i].info.client_ip() == clientinfo.client_ip()) {
            return clients[i].id.id();
        }
    }

    return -1;
}


int generateFileDescriptor() {
    fd_counter++;; 
    return fd_counter;
}

void acquireToken(struct heldorrequestedtokenbyclient h) {
    //make sure this is called when tokenconsistency loc is held
    tokensAcquiredclients.push_back(h);
}


bool checkConflicts(struct heldorrequestedtokenbyclient h) {
    //first check whether already held by others
    //then check whether this is conflicted with pending requested tokens
    //make sure this is called when tokenconsistency loc is held
    for (int i = 0; i < (int)tokensAcquiredclients.size(); i++) {
        if (conflict(h, tokensAcquiredclients[i])) {
            return true;
        }
    }

    for (int i = 0; i < (int)tokensRequestedclients.size(); i++) {
        if (conflict(h, tokensRequestedclients[i]->r)) {
            return true;
        }
    }

    return false;
}

vector<int> allocateServers(int width) {
    //simple algo for now
    vector<int> servers;
    for (int i = 0; i < width; i++) servers.push_back(i);
    return servers;
}

bool cbreadTypeAll (vector<int> cids, vector<int> ptypes, int clientid) {
    for (int i = 0; i < (int)cids.size(); i++) {
        if (clientid != cids[i] && ptypes[i] == 2) return false;
    }

    return true;
}

void invalidateCb(struct CacheBlock cb, vector<int> cids, int clientid) {
    for (int i = 0; i < (int)cids.size(); i++) {
        if (cids[i] != clientid) {
            //request send to invalidae this cb
            //cout<<cids[i] << "///" << clientid;

            c[cids[i]]->invalidateCacheBlock(cb.fd, cb.blocknum);

        }
    }
}

class ServerImpl final : public MetaService::Service {
public:
    Status getClientId(ServerContext* context, const ClientInfo* request, ClientId* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;
        
        mtx.lock();
        //first search whether this client is already present.
        /*int id = findClient(*request);
        if(id != -1) {
            response->set_id(id);

            mtx.unlock();
            return Status::OK;
        }*/

        
        client_counter++;
        response->set_id(client_counter);
        ClientData client; client.info = *request; client.id = *response;
        clients.push_back(client);        

        mtx.unlock();

        return Status::OK;
    }

    Status startClientService(ServerContext* context, const ClientInfo* request, Empty* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;
        //cout<<request->client_ip() + ":" + to_string(60000 + request->clientid())<<endl;
        c[request->clientid()] = new Client(grpc::CreateChannel(request->client_ip() + ":" + to_string(60000 + request->clientid()), grpc::InsecureChannelCredentials()));
        
        //int ok = c[request->clientid()]->isConnected(*request);
        //cout << "OK " <<ok<<endl;

        return Status::OK;
    }


    Status createFile(ServerContext* context, const FileCreateRequest* request, ResponseCode* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        string filename = request->filename();
        int stripe_width = request->stripe_width();

        fileMetadatasMtx.lock();

        auto it = fileMetadatas.find(filename);
        if (it != fileMetadatas.end()) {
            response->set_val(-1);
        } else {
            response->set_val(0);
            struct pfs_metadata fileMetadata;
            strcpy(fileMetadata.filename, filename.c_str());
            fileMetadata.file_size = 0;
            fileMetadata.ctime = getCurTime();
            fileMetadata.mtime = fileMetadata.ctime;
            
            struct pfs_filerecipe recipe;
            recipe.servers = allocateServers(stripe_width);
            recipe.stripe_width = stripe_width;
            fileMetadata.recipe = recipe;
            fileMetadata.accessed = 0;

            //set FD
            int fileDescriptor = generateFileDescriptor();
            fileMetadata.fd = fileDescriptor;
            fdToFilenameMap[fileDescriptor] = filename; // do I need lock for this??

            //insert
            fileMetadatas[filename] = fileMetadata;

            //std::cout << fileMetadata <<std::endl;
        }

        fileMetadatasMtx.unlock(); 
        
        return Status::OK;
    }

    Status deleteFile(ServerContext* context, const FileDeleteRequest* request, Sidlist* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        string filename = request->filename();
        response->set_res(0);
        int fd = -1;
        fileMetadatasMtx.lock();

        auto it = fileMetadatas.find(filename);
        if (it == fileMetadatas.end()) {
            response->set_res(-1);
        } else {
            struct pfs_metadata md = fileMetadatas[filename];
            if (md.accessed > 0) response->set_res(-1);
            else {
                response->set_fd(md.fd);
                for (int i = 0; i < (int)md.recipe.servers.size(); i++) {
                    response->add_sids(md.recipe.servers[i]);
                }
                fd = md.fd;
                fdToFilenameMap.erase(fd);
                fileMetadatas.erase(filename); 
            }
        }

        fileMetadatasMtx.unlock(); 

        //cachemap remove
        if (fd != -1) {
            cachemapconsistency.lock();
            for (auto it = cachemap.begin(); it != cachemap.end(); ++it) {
                struct CacheBlock cb = it->first;
                if (cb.fd == fd) cachemap.erase(cb);
            }

            cachemapconsistency.unlock();
        }
        
        return Status::OK;
    }


    Status openFile(ServerContext* context, const FileOpenRequest* request, ResponseCode* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        string filename = request->filename();
        //std::cout << filename << std::endl;
        int val = -1;

        fileMetadatasMtx.lock();

        auto it = fileMetadatas.find(filename);
        if (it != fileMetadatas.end()) {
            val = fileMetadatas[filename].fd;
            fileMetadatas[filename].accessed++;
        } else {
            //std::cerr << "File not found: " << filename << std::endl;
        }

        fileMetadatasMtx.unlock();

        response->set_val(val);
        return Status::OK;
    }

    Status closefd(ServerContext* context, const CloseRequest* request, Empty* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        int fd = request->fd();

        fileMetadatasMtx.lock();

        string filename = fdToFilenameMap[fd];

        auto it = fileMetadatas.find(filename);
        if (it != fileMetadatas.end()) {
            fileMetadatas[filename].accessed--;
        } else {
            //std::cerr << "File not found: " << filename << std::endl;
        }

        fileMetadatasMtx.unlock();

        tokenconsistency.lock();
        //cout<<"removing tokens"<<endl;
        removeTokens(fd, request->clientid());
        //printtokenlistMeta(tokensAcquiredclients);
        //cout<<"removing tokens finished"<<endl;
        tokenconsistency.unlock();

        //cachemap remove
        if (CACHE_ENABLED) {
            removeClientFromCBMap(fd, request->clientid());
        }

        return Status::OK;
    }

    Status fstat(ServerContext* context, const ResponseCode* request, Metadata* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        int fd = request->val();
        response->set_res(-1);
        fileMetadatasMtx.lock();

        if (fdToFilenameMap.find(fd) != fdToFilenameMap.end()) {
            string filename = fdToFilenameMap[fd];

            auto it = fileMetadatas.find(filename);
            if (it != fileMetadatas.end()) {
                response->set_res(0);

                struct pfs_metadata md = fileMetadatas[filename];
                response->set_fname(md.filename);
                response->set_filesize(md.file_size);
                response->set_ctime(md.ctime);
                response->set_mtime(md.mtime);
            } else {
                //std::cerr << "File not found: " << filename << std::endl;
            }
        }

        fileMetadatasMtx.unlock();

        
        return Status::OK;
    }

    Status getfilesize(ServerContext* context, const File* request, File* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        int fd = request->fd();
        response->set_filesize(-1);
        fileMetadatasMtx.lock();

        if (fdToFilenameMap.find(fd) != fdToFilenameMap.end()) {
            string filename = fdToFilenameMap[fd];

            auto it = fileMetadatas.find(filename);
            if (it != fileMetadatas.end()) {
                struct pfs_metadata md = fileMetadatas[filename];
                response->set_filesize(md.file_size);
            } else {
                //std::cerr << "File not found: " << filename << std::endl;
            }
        }

        fileMetadatasMtx.unlock();

        
        return Status::OK;
    }

    Status setfilesize(ServerContext* context, const File* request, File* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        int fd = request->fd();
        ll filesize = request->filesize();
        fileMetadatasMtx.lock();

        if (fdToFilenameMap.find(fd) != fdToFilenameMap.end()) {
            string filename = fdToFilenameMap[fd];

            auto it = fileMetadatas.find(filename);
            if (it != fileMetadatas.end()) {
                struct pfs_metadata* md = &(fileMetadatas[filename]);
                //cout<<md->file_size << " "<<filesize<<endl;
                if ((ll)md->file_size < filesize) { //uint64??
                    md->file_size = filesize;
                }

                md->mtime = getCurTime();
            } else {
                //std::cerr << "File not found: " << filename << std::endl;
            }
        }

        fileMetadatasMtx.unlock();

        
        return Status::OK;
    }


    Status getServerInfoForBlock(ServerContext* context, const Blockinfo* request, ResponseCode* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        int fd = request->fd();
        ll blocknum = request->blocknum();

        fileMetadatasMtx.lock(); // do we need this lock? I think no--revisit later
        string filename = fdToFilenameMap[fd];
        struct pfs_filerecipe recipe = fileMetadatas[filename].recipe;
        
        int sid = recipe.getSid(blocknum);
        
        fileMetadatasMtx.unlock();

        response->set_val(sid); //serverid 0:MAX-1
        return Status::OK;
    }

    Status canCacheBlock(ServerContext* context, const CacheRequest* request, ResponseCode* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        Blockinfo block = request->block();
        struct CacheBlock cb; cb.fd = block.fd(); cb.blocknum = block.blocknum();

        cachemapconsistency.lock(); //cache DS lock

        if (cachemap.find(cb) == cachemap.end()) {
            cachemap[cb] = new struct CacheInfo();
        }

        struct CacheInfo* cinfo = cachemap[cb];

        cachemapconsistency.unlock();

        // BLOCK lock
        cinfo->mtx.lock();

        int canCache = 1; //full block

        if (request->full() == 0) { // full == 0
            if (request->type() == 1) { // read miss
                if (!cbreadTypeAll(cinfo->cids, cinfo->ptypes, request->clientid())) canCache = 0;
            } else { //write miss
                invalidateCb(cb, cinfo->cids, request->clientid());
                cinfo->cids.clear();
                cinfo->ptypes.clear();
            }
        }

        if (canCache) {
            //update
            cinfo->cids.push_back(request->clientid());
            cinfo->ptypes.push_back(request->type());
        }


        cinfo->mtx.unlock();


        response->set_val(canCache);
        
        return Status::OK;
    }

    Status requestToken(ServerContext* context, const Token* request, ResponseCode* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        vector<struct tokeninterval> tokens;
        for (int i = 0; i < request->tokenintervals().size(); i++) {
            struct tokeninterval ti;
            ti.startoffset = request->tokenintervals(i).startoffset();
            ti.endoffset = request->tokenintervals(i).endoffset();
            tokens.push_back(ti);
        }

        heldorrequestedtokenbyclient h;
        h.fd = request->fd();
        h.clientid = request->clientid();
        h.type = request->type();
        h.tokens = tokens;

        tokenconsistency.lock();

        //sent client to set unlocm of its tokenconcsitency lock
        //int st = c[h.clientid]->setUnlock();
        //if (st != 0) cout<<"error" <<endl;

        int resp = checkConflicts(h);
        if (resp) {

            cout<< "conflicts!" << endl;
            //sent client to set unlocm of its tokenconcsitency lock
            /*int st = c[h.clientid]->setUnlock();
            if (st != 0) cout<<"error" <<endl;*/

            // put this in the requested list. and make this wait until conflicted tokens are revoked.
            //after then, it will be awake.
            struct requestedtokenbyclient* r = new struct requestedtokenbyclient();
            r->r = h;

            tokensRequestedclients.push_back(r);
            r->mlock.lock();

            sem_post(&s);

            tokenconsistency.unlock(); // its an critical section, so, needs to unlock before going to wait mode.

            r->mlock.lock(); //wait

            //cout<< "revocation successful;" <<endl;

            /*tokenconsistency.lock();

            //acquireToken(heldorrequestedtokenbyclient);  //token is actaully accquired before unlocking this. so, just return now to client.
    
            tokenconsistency.unlock();*/

            r->mlock.unlock();

            response->set_val(0);
            //cout<<"---------"<<endl;
            //printtokenlistMeta(tokensAcquiredclients);
            //cout<<"---------"<<endl;

            return Status::OK;
        } else { // non-conflict, so, tokenconsistency lock is still held
        
        //acquire token. this will be done whenever pendingProcess unlock this which ensures that conflickts is resloved.

            acquireToken(h);

            //send client that u should not revoke tokens until your current operation finished.
            //int status = 
            //cout<<request->clientid()<<endl;

            c[request->clientid()]->stopRevocation();


            //cout<<"---------"<<endl;
            //printtokenlistMeta(tokensAcquiredclients);
            //cout<<"---------"<<endl;

            tokenconsistency.unlock();

            response->set_val(0);
            return Status::OK;
        }
    }

};

void RunServer(string server_address) {
    ServerImpl service;

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Meta Server listening on " << server_address << std::endl;
    server->Wait();
}





int main(int argc, char *argv[])
{
    string hostname = getMyIP().c_str();// it needs to be changed later on when we put IP address in pfs_file.txt
    printf("%s:%s: Start! Hostname: %s, IP: %s\n", __FILE__, __func__, getMyHostname().c_str(), getMyIP().c_str());
    
    // Run the PFS metadata server and listen to requests
    // Check if my IP is in the first line of pfs_list.txt
    if (!isMetaServer(hostname)) {
        printf("I am not listed as MetaServer. exiting\n");
        return 0;
    }

    string serverAddress = getMyPort(hostname);

    cout<< "Meta server " << serverAddress << " Up!" <<endl;



    sem_init(&s, 1, 0);
    sem_init(&p, 1, 0);
    thread pendingProcessThread(pendingTokenRequestProcess);
    pendingProcessThread.detach();
    
    RunServer(serverAddress);

    
    
    printf("%s:%s: Finish!\n", __FILE__, __func__);
    return 0;
}
