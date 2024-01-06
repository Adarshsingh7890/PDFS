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


vpath %.o ../

.PHONY: default clean
default: sample sample1-1 sample1-2 sample1-3

sample: sample.cpp pfs_api.o pfs_common.o pfs_client.pb.o pfs_client.grpc.pb.o
	$(CXX) $(CXXFLAGS) -I../ $(INC_DIR) $^ $(LDFLAGS) -o $@
sample1-1: sample1-1.cpp pfs_api.o pfs_common.o pfs_client.pb.o pfs_client.grpc.pb.o
	$(CXX) $(CXXFLAGS) -I../ $(INC_DIR) $^ $(LDFLAGS) -o $@
sample1-2: sample1-2.cpp pfs_api.o pfs_common.o pfs_client.pb.o pfs_client.grpc.pb.o
	$(CXX) $(CXXFLAGS) -I../ $(INC_DIR) $^ $(LDFLAGS) -o $@
sample1-3: sample1-3.cpp pfs_api.o pfs_common.o pfs_client.pb.o pfs_client.grpc.pb.o
	$(CXX) $(CXXFLAGS) -I../ $(INC_DIR) $^ $(LDFLAGS) -o $@

clean:
	rm -f sample sample1-1 sample1-2 sample1-3 output.txt
