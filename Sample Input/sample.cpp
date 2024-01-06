#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "pfs_api.hpp"
#include "pfs_common.hpp"


int main(int argc, char *argv[])
{
    printf("%s:%s: Start! Hostname: %s, IP: %s\n", __FILE__, __func__, getMyHostname().c_str(), getMyIP().c_str());
    if (argc < 2)
    {
        fprintf(stderr, "%s: usage: ./sample1-1 <input filename>\n", __func__);
        return -1;
    }

    // Initialize the PFS client
    int client_id = pfs_initialize();
    if (client_id == -1)
    {
        fprintf(stderr, "pfs_initialize() failed.\n");
        return -1;
    }

    printf("client_id: %d\n", client_id);

    if (client_id != pfs_initialize()) {
        fprintf(stderr, "pfs_initialize() return different id for same client.\n");
        return -1;
    }

    // Create a PFS file
    int ret;
    ret = pfs_create("pfs_file1", 10);
    if (ret == -1)
    {
        fprintf(stderr, "Unable to create a PFS file as file server is less than stripe width.\n");
        //return -1;
    }

    ret = pfs_create("pfs_file1", 1);
    if (ret == -1)
    {
        fprintf(stderr, "Unable to create a PFS file.\n");
        //return -1;
    }

    ret = pfs_create("pfs_file1", 2);
    if (ret == -1)
    {
        fprintf(stderr, "Unable to create a PFS file. duplicate\n");
        //return -1;
    }

    // Open the PFS file in write mode
    int pfs_fd = pfs_open("pfs_file2", 2);
    if (pfs_fd == -1)
    {
        fprintf(stderr, "no such PFS file.\n");
        //return -1;
    }

    // Open the PFS file in write mode
    pfs_fd = pfs_open("pfs_file1", 2);
    if (pfs_fd == -1)
    {
        fprintf(stderr, "Error opening PFS file.\n");
        //return -1;
    }

    // Open a file
    std::string input_filename(argv[1]);
    int input_fd = open(input_filename.c_str(), O_RDONLY);
    char *buf = (char *)malloc(8 * 1024);

    ssize_t nread = pread(input_fd, (void *)buf, 1024, 0);
    if (nread != 1024)
    {
        fprintf(stderr, "pread() error.\n");
        return -1;
    }
    close(input_fd);

    ret = pfs_write(pfs_fd, (void *)buf, 10, 0);
    if (ret == -1)
    {
        fprintf(stderr, "Write error to PFS file.\n");
        return -1;
    }

    size_t pfs_buf_size = 16 * 1024;
    char *pfs_buf = (char *)malloc(pfs_buf_size);
    memset(pfs_buf, 0, pfs_buf_size);
    ret = pfs_read(pfs_fd, (void *)pfs_buf, 10, 0);

    int cmp = memcmp(buf, pfs_buf, 10);
    if (cmp != 0)
    {
        printf("myfile.txt and PFS file memcmp() failed.\n");
        // return -1; // THIS IS AN ERROR, but continue for explanation purposes
    }

    /*
    getWriteToken(pfs_fd, 0, 10);
    getWriteToken(pfs_fd, 0, 10);
    getWriteToken(pfs_fd, 8, 12);


    while(1) {
        int op;
        scanf("%d", &op);
        printf("%d pressed\n", op);
        if (op == 1) {
            getWriteToken(pfs_fd, 8, 12);
        } else {
            getReadToken(pfs_fd, 8, 12);
            getReadToken(pfs_fd, 8, 12);
        }
    }*/

    // Open the PFS file in read mode
    pfs_fd = pfs_open("pfs_file1", 1);
    if (pfs_fd == -1)
    {
        fprintf(stderr, "file is already opened.\n");
        //return -1;
    }

    return 0;
}
