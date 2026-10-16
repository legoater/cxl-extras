// SPDX-License-Identifier: GPL-2.0
/*
 * CXL Type-2 Accelerator Test
 *
 * Demonstrates the type-2 accelerator flow:
 *   1. mmap CXL device memory via /dev/dax
 *   2. Write input data (from file or generated pattern)
 *   3. Submit compress command via ioctl
 *   4. Submit decompress command to verify roundtrip
 *   5. Compare original and decompressed data
 *
 * Usage: cxl_accel_test [options] [input_file]
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <errno.h>

#include "cxl_type2.h"

#define DEFAULT_ACCEL_DEV	"/dev/cxl-accel0"
#define DEFAULT_DAX_DEV		"/dev/dax0.0"
#define DEFAULT_SIZE_KB		4
#define ALIGN_UP(x, a)		(((x) + (a) - 1) & ~((a) - 1))

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [options] [input_file]\n"
		"\n"
		"Options:\n"
		"  -a <dev>   Accelerator device (default: %s)\n"
		"  -d <dev>   DAX device for CXL memory (default: %s)\n"
		"  -s <kb>    Pattern size in KB when no file given (default: %d)\n"
		"  -h         Show this help\n"
		"\n"
		"If input_file is given, its contents are compressed.\n"
		"Otherwise a synthetic pattern is generated.\n",
		prog, DEFAULT_ACCEL_DEV, DEFAULT_DAX_DEV, DEFAULT_SIZE_KB);
}

static int submit_cmd(int accel_fd, uint32_t opcode,
		      uint64_t src_off, uint64_t src_len,
		      uint64_t dst_off, uint64_t dst_len,
		      uint64_t *result_len)
{
	struct cxl_accel_cmd cmd = {
		.opcode     = opcode,
		.src_offset = src_off,
		.src_length = src_len,
		.dst_offset = dst_off,
		.dst_length = dst_len,
	};
	int rc;

	rc = ioctl(accel_fd, CXL_ACCEL_IOCTL_SUBMIT, &cmd);
	if (rc < 0) {
		perror("ioctl CXL_ACCEL_IOCTL_SUBMIT");
		return -1;
	}

	*result_len = cmd.result_len;
	return 0;
}

static size_t load_file(const char *path, uint8_t *dst, size_t max)
{
	FILE *fp;
	size_t n;

	fp = fopen(path, "rb");
	if (!fp) {
		perror(path);
		return 0;
	}

	n = fread(dst, 1, max, fp);
	fclose(fp);
	return n;
}

static int dax_read_sysfs_u64(const char *dax_path, const char *attr,
			      uint64_t *val)
{
	char path[256];
	const char *name;
	FILE *fp;

	name = strrchr(dax_path, '/');
	name = name ? name + 1 : dax_path;

	snprintf(path, sizeof(path), "/sys/bus/dax/devices/%s/%s", name, attr);
	fp = fopen(path, "r");
	if (!fp || fscanf(fp, "%" SCNu64, val) != 1) {
		fprintf(stderr, "Cannot read %s for %s\n", attr, dax_path);
		if (fp)
			fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

static ssize_t file_size(const char *path)
{
	struct stat st;

	if (stat(path, &st) < 0) {
		perror(path);
		return -1;
	}
	if (!st.st_size) {
		fprintf(stderr, "%s: empty file\n", path);
		return -1;
	}
	return st.st_size;
}

static void fill_pattern(uint8_t *buf, size_t size)
{
	const char *pattern = "CXL type-2 accelerator test data. ";
	size_t plen = strlen(pattern);

	for (size_t i = 0; i < size; i++)
		buf[i] = pattern[i % plen];
}

int main(int argc, char **argv)
{
	const char *accel_path = DEFAULT_ACCEL_DEV;
	const char *dax_path = DEFAULT_DAX_DEV;
	const char *input_file = NULL;
	size_t data_size = DEFAULT_SIZE_KB * 1024;
	size_t region_size, max_out;
	uint64_t src_off, comp_off, decomp_off;
	uint64_t comp_len, decomp_len;
	int accel_fd, dax_fd;
	uint8_t *mem;
	uint64_t dax_size = 0, dax_align = 0;
	int opt, rc = 1;

	while ((opt = getopt(argc, argv, "a:d:s:h")) != -1) {
		switch (opt) {
		case 'a':
			accel_path = optarg;
			break;
		case 'd':
			dax_path = optarg;
			break;
		case 's':
			data_size = atoi(optarg) * 1024;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind < argc)
		input_file = argv[optind];

	/* Open accelerator device */
	accel_fd = open(accel_path, O_RDWR);
	if (accel_fd < 0) {
		perror(accel_path);
		return 1;
	}

	/* Open and mmap DAX device */
	dax_fd = open(dax_path, O_RDWR);
	if (dax_fd < 0) {
		perror(dax_path);
		goto out_accel;
	}

	/* fstat returns 0 for DAX char devices; read size/align from sysfs */
	if (dax_read_sysfs_u64(dax_path, "size", &dax_size) || !dax_size)
		goto out_dax;
	if (dax_read_sysfs_u64(dax_path, "align", &dax_align) || !dax_align)
		goto out_dax;

	/*
	 * If a file is given, read it first to know the size.
	 * We need to mmap enough CXL memory for 3 regions:
	 *   [0, data_size)              - input
	 *   [data_size, 2*data_size)    - compressed
	 *   [2*data_size, 3*data_size)  - decompressed
	 *
	 * For the compressed region, allow up to data_size + 1K
	 * headroom since incompressible data can expand slightly.
	 */
	if (input_file) {
		ssize_t sz = file_size(input_file);

		if (sz < 0)
			goto out_dax;
		data_size = sz;
	}

	max_out = data_size + (data_size >> 12) + (data_size >> 14) + 64;
	region_size = data_size + max_out + data_size;
	src_off = 0;
	comp_off = data_size;
	decomp_off = data_size + max_out;

	region_size = ALIGN_UP(region_size, dax_align);

	if (dax_size < region_size) {
		fprintf(stderr, "DAX device too small: %" PRIu64 " < %zu\n",
			dax_size, region_size);
		goto out_dax;
	}

	mem = mmap(NULL, region_size, PROT_READ | PROT_WRITE,
		   MAP_SHARED, dax_fd, 0);
	if (mem == MAP_FAILED) {
		perror("mmap dax");
		goto out_dax;
	}

	/* Write input data to CXL memory */
	if (input_file) {
		size_t n = load_file(input_file, mem + src_off, data_size);

		if (n != data_size) {
			fprintf(stderr, "Short read: %zu/%zu\n", n, data_size);
			goto out_unmap;
		}
		printf("Loaded %zu bytes from %s\n", data_size, input_file);
	} else {
		fill_pattern(mem + src_off, data_size);
		printf("Generated %zu bytes of test pattern\n", data_size);
	}

	/* Clear output regions */
	memset(mem + comp_off, 0, max_out);
	memset(mem + decomp_off, 0, data_size);

	/* Compress */
	printf("Compressing: [0x%" PRIx64 ", %zu] -> [0x%" PRIx64 ", %zu]...\n",
	       src_off, data_size, comp_off, max_out);
	if (submit_cmd(accel_fd, CXL_ACCEL_OP_COMPRESS,
		       src_off, data_size,
		       comp_off, max_out,
		       &comp_len) < 0)
		goto out_unmap;

	printf("Compressed: %zu -> %" PRIu64 " bytes (%.1f%%)\n",
	       data_size, comp_len,
	       100.0 * (double)comp_len / (double)data_size);

	/* Decompress */
	printf("Decompressing: [0x%" PRIx64 ", %" PRIu64 "] -> [0x%" PRIx64 ", %zu]...\n",
	       comp_off, comp_len, decomp_off, data_size);
	if (submit_cmd(accel_fd, CXL_ACCEL_OP_DECOMPRESS,
		       comp_off, comp_len,
		       decomp_off, data_size,
		       &decomp_len) < 0)
		goto out_unmap;

	printf("Decompressed: %" PRIu64 " -> %" PRIu64 " bytes\n",
	       comp_len, decomp_len);

	/* Verify */
	if (decomp_len != data_size) {
		fprintf(stderr, "FAIL: size mismatch: expected %zu, got %" PRIu64 "\n",
			data_size, decomp_len);
		goto out_unmap;
	}

	if (memcmp(mem + src_off, mem + decomp_off, data_size) != 0) {
		fprintf(stderr, "FAIL: data mismatch after roundtrip\n");
		goto out_unmap;
	}

	printf("PASS: compress/decompress roundtrip verified\n");
	rc = 0;

out_unmap:
	munmap(mem, region_size);
out_dax:
	close(dax_fd);
out_accel:
	close(accel_fd);
	return rc;
}
