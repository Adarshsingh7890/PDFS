PFS_GRPC_DIR = /home/other/CSE511-FA23/grpc
GCC_DIR = /home/software/gcc/gcc-11.3.0
INC_DIR = -I$(PFS_GRPC_DIR)/include

CXX = $(GCC_DIR)/bin/g++
CXXFLAGS = -std=c++11 -Wall $(INC_DIR)
LDFLAGS = -Wl,--copy-dt-needed-entries \
          -L$(GCC_DIR)/lib64 \
		  -L$(PFS_GRPC_DIR)/lib64 -L$(PFS_GRPC_DIR)/lib \
		  -pthread -lprotobuf -lgrpc++ \
          -Wl,--no-as-needed -lgrpc++_reflection -Wl,--no-as-needed -ldl -labsl_synchronization -lssl -lcrypto

.PHONY: default clean
.SECONDARY:
default: pfs_metaserver pfs_fileserver pfs_client.pb.o pfs_client.grpc.pb.o pfs_api.o 

pfs_metaserver: pfs_metaserver.pb.o pfs_metaserver.grpc.pb.o pfs_metaserver.o pfs_common.o
	$(CXX) $(CXXFLAGS) $(INC_DIR) $^ $(LDFLAGS) -o $@

pfs_fileserver: pfs_fileserver.pb.o pfs_fileserver.grpc.pb.o pfs_fileserver.o pfs_common.o
	$(CXX) $(CXXFLAGS) $(INC_DIR) $^ $(LDFLAGS) -o $@


# Protobuf
PROTOC = $(LD_LIBRARY_DIR) $(PFS_GRPC_DIR)/bin/protoc
GRPC_CPP_PLUGIN_PATH ?= $(PFS_GRPC_DIR)/bin/grpc_cpp_plugin
PROTOS_PATH = .
%.grpc.pb.o: %.proto
	$(PROTOC) -I$(PROTOS_PATH) --grpc_out=. --plugin=protoc-gen-grpc=$(GRPC_CPP_PLUGIN_PATH) $<
	$(CXX) -I./ -I/home/other/CSE511-FA23/grpc/include/ -c $*.grpc.pb.cc -std=c++11
%.pb.o: %.proto
	$(PROTOC) -I$(PROTOS_PATH) --cpp_out=. $<
	$(CXX) -I./ -I/home/other/CSE511-FA23/grpc/include/ -c $*.pb.cc -std=c++11

clean:
	rm -f pfs_metaserver pfs_fileserver *.o *.pb.cc *.pb.h