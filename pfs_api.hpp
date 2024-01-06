#pragma once

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstdbool>
#define ll long long int
#include "pfs_config.hpp"

int pfs_initialize();
int pfs_create(const char *filename, int stripe_width);
int pfs_open(const char *filename, int mode);
int pfs_read(int fd, void *buf, size_t num_bytes, off_t offset);
int pfs_write(int fd, const void *buf, size_t num_bytes, off_t offset);
int pfs_close(int fd);
int pfs_delete(const char *filename);
int pfs_fstat(int fd, struct pfs_metadata *meta_data);
int pfs_execstat(struct pfs_execstat *execstat_data);

// this functions will be removed later
int getReadToken(int fd, ll startoffset, ll endoffset);
int getWriteToken(int fd, ll startoffset, ll endoffset);
