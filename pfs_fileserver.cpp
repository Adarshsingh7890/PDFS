#include "pfs_fileserver.hpp"
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <fstream>
#include <string>
#include <glob.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using google::protobuf::Empty;



using namespace std;
string getFilePath(int fd, ll blocknum) {
    string filename =  "/scratch/" + to_string(fd) + "-" + to_string(blocknum);
    return filename;

}

string getFilePathPattern(int fd) {
    return  "/scratch/" + to_string(fd) + "-*";
}


void deleteAllBlocks(int fd) {
    const char* pattern = getFilePathPattern(fd).c_str();

    // Initialize a glob structure
    glob_t glob_result;

    // Perform the globbing
    if (glob(pattern, GLOB_MARK | GLOB_NOSORT, nullptr, &glob_result) == 0) {
        // Iterate through the matching files and delete them
        for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
            const char* filename = glob_result.gl_pathv[i];

            // Attempt to delete the file
            if (std::remove(filename) != 0) {
                //perror("Error deleting file");
            } else {
                //std::cout << "File deleted: " << filename << std::endl;
            }
        }

        // Free the memory allocated by glob
        globfree(&glob_result);
    } else {
        //std::cerr << "Error in globbing" << std::endl;
    }
}



class ServerImpl final : public FileService::Service {
public:
    Status isConnected(ServerContext* context, const Dummy* request, Empty* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;
        return Status::OK;
    }

    Status deleteFd(ServerContext* context, const File* request, Empty* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;
        int fd = request->fd();

        deleteAllBlocks(fd);
        return Status::OK;
    }

    Status readData(ServerContext* context, const Blockinfo* request, Data* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        int fd = request->fd();
        ll blocknum = request->blocknum();
        string filePath = getFilePath(fd, blocknum);

        std::ifstream inFile(filePath, std::ios::binary);
        if (inFile.is_open()) {
            // Get the size of the file
            inFile.seekg(0, std::ios::end);
            std::streampos fileSize = inFile.tellg();
            inFile.seekg(0, std::ios::beg);

            // Read the binary data into a string
            std::string binaryData(fileSize, '\0');
            inFile.read(&binaryData[0], fileSize);
            //cout<<"FS:" <<binaryData<<endl;

            inFile.close();

            response->set_data(binaryData);
        } else {
            string s;
            response->set_data(s);
        }

        response->set_fd(fd); response->set_blocknum(blocknum);

        return Status::OK;
    }

    Status writeData(ServerContext* context, const Data* request, Empty* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;
        int fd = request->fd();
        ll blocknum = request->blocknum();
        string filePath = getFilePath(fd, blocknum);

        int fileDescriptor = open(filePath.c_str(), O_RDWR | O_CREAT, 0600);
        if (fileDescriptor == -1) {
            return Status::CANCELLED;
        }

        string data = request->data();
        //cout<<"FS got" << data <<endl;
        pwrite(fileDescriptor, data.c_str(), PFS_BLOCK_SIZE, 0);

        close(fileDescriptor);
        return Status::OK;
    }

    Status writeDataSpecific(ServerContext* context, const DataSpecific* request, Empty* response) override {
        //std::cout << "Received request: " << request->DebugString() << std::endl;

        int fd = request->fd();
        ll blocknum = request->blocknum();
        string filePath = getFilePath(fd, blocknum);

        int fileDescriptor = open(filePath.c_str(), O_RDWR | O_CREAT, 0600);
        if (fileDescriptor == -1) {
            return Status::CANCELLED;
        }

        string data = request->data();
        //cout<<"FS got" << data <<endl;
        pwrite(fileDescriptor, data.c_str(), request->size(), request->startoffset());

        close(fileDescriptor);

        return Status::OK;
    }
};



void RunServer(string server_address) {
    ServerImpl service;

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "File Server listening on " << server_address << std::endl;
    server->Wait();
}


int main(int argc, char *argv[])
{
    string hostname = getMyIP().c_str(); // it needs to be changed later on when we put IP address in pfs_file.txt

    printf("%s:%s: Start! Hostname: %s, IP: %s\n", __FILE__, __func__, getMyHostname().c_str(), getMyIP().c_str());
    
    // Run the PFS fileserver and listen to requests
    // Check if my IP is in pfs_list.txt
    if (!isFileServer(hostname)) {
        printf("I am not listed as FileServer. exiting\n");
        return 0;
    }

    string serverAddress = getMyPort(hostname);

    cout<< "File server " << serverAddress << " Up!" <<endl;

    RunServer(serverAddress);
    
    printf("%s:%s: Finish!\n", __FILE__, __func__);
    return 0;
}
