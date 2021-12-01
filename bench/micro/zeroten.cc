#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <err.h>
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <assert.h>


#include <iostream>
#include <string>
#include <vector>
#include <list>
#include <random>
#include <memory>
#include <fstream>

#if 0
#include <boost/random/uniform_real_distribution.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/variate_generator.hpp>
#include <boost/math/distributions/pareto.hpp>
#include <boost/random.hpp>
#endif

#include "time_stat.h"
#include "thread.h"

const char *test_dir_prefix = "/mlfs/";

char *test_file_name = "testfile";
#define thread0_off 0UL
#define thread1_off 1048756UL  // 1GB
#define thread2_off 2097152UL // 2GB

//uncomment below to run strawman
//#define USER_BLOCK_MIGRATION 1

#ifdef USER_BLOCK_MIGRATION
#include "batch_migration.h"
#define open(x,y,z) bb_open(x,y,z)
//#define open(x,y) bb_open(x,y)
#define read(x,y,z) bb_read(x,y,z)
#define write(x,y,z) bb_write(x,y,z)
#define pread(x,y,z,w) bb_pread(x,y,z,w)
#define fsync(x) bb_fsync(x)
#endif

#define ALIGN_MASK(x, mask) (((x) + (mask)) & ~(mask))
#define ALIGN_MASK_FLOOR(x, mask) (((x)) & ~(mask))
#define ALIGN(x, a)  ALIGN_MASK((x), ((__typeof__(x))(a) - 1))
#define ALIGN_FLOOR(x, a)  ALIGN_MASK_FLOOR((x), ((__typeof__(x))(a) - 1))
#define BUF_SIZE (2 << 20)

//#define ODIRECT
#undef ODIRECT
// #define VERIFY

typedef enum {SEQ_WRITE, SEQ_READ, SEQ_WRITE_READ, RAND_WRITE, RAND_READ, 
	ZIPF_WRITE, ZIPF_READ, ZIPF_MIX, NONE} test_t;

typedef enum {FS} test_mode_t;

typedef unsigned long addr_t;

static pthread_barrier_t tsync;
uint8_t dev_id;
unsigned long ops_cap;
int psync; //process barrier
int do_fsync;



class io_bench : public CThread 
{
	public:
		io_bench(int _id, unsigned long _file_size_bytes, unsigned int _io_size,
				test_t _test_type, string _zipf_file);

		int id, fd, per_thread_stats;
		unsigned long file_size_bytes;
		unsigned int io_size;
		test_t test_type;
		string test_file;
		string zipf_file;
		char *buf;
		struct time_stats stats;

		std::list<uint64_t> io_list;
        std::list<uint8_t> op_list;

		pthread_cond_t cv;
		pthread_mutex_t cv_mutex;

		void prepare(void);
		void cleanup(void);

		void do_read(void);
		void do_write(void);

		// Thread entry point.
		void Run(void);

		// util methods
		static unsigned long str_to_size(char* str);
		static string get_test_str(test_t test);
		static test_t get_test_type(char *);
		static test_mode_t get_test_mode(char *);
		static void hexdump(void *mem, unsigned int len);
		static void show_usage(const char *prog);
};

io_bench::io_bench(int _id, unsigned long _file_size_bytes, 
		unsigned int _io_size, test_t _test_type, string _zipf_file)
	: id(_id), file_size_bytes(_file_size_bytes), io_size(_io_size), 
	test_type(_test_type), zipf_file(_zipf_file)
{
	test_file.assign(test_dir_prefix);
	//test_file += "/file" + std::to_string(dev_id) + "-" + std::to_string(id) + std::to_string(getpid());
	test_file += "test_file";
	// test_file += "/" + std::string(test_file_name) + std::to_string(id) + "-" + std::to_string(dev_id);
	per_thread_stats = 0;
}

#define handle_error_en(en, msg) \
	do { errno = en; perror(msg); exit(EXIT_FAILURE); } while (0)

#define SHM_PATH "/iobench_shm"
//#define SHM_F_SIZE 1024
#define SHM_F_SIZE 128
// #define OUTPUT_TO_FILE 1     // write output to file not stdout.

void* create_shm(int &fd, int &res) {
	void * addr;
	fd = shm_open(SHM_PATH, O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd < 0) {
		exit(-1);
	}

	res = ftruncate(fd, SHM_F_SIZE);
	if (res < 0)
	{
		exit(-1);
	}

	addr = mmap(NULL, SHM_F_SIZE, PROT_WRITE, MAP_SHARED, fd, 0);
	if (addr == MAP_FAILED){
		exit(-1);
	}

	return addr;
}

void destroy_shm(void *addr) {
	int ret, fd;
	ret = munmap(addr, SHM_F_SIZE);
	if (ret < 0)
	{
		exit(-1);
	}

	fd = shm_unlink(SHM_PATH);
	if (fd < 0) {
		exit(-1);
	}
}
void io_bench::prepare(void)
{
	int ret, s;
	cpu_set_t cpuset;
	off_t off_th;
	pthread_mutex_init(&cv_mutex, NULL);
	pthread_cond_init(&cv, NULL);

	fd = open("/mlfs/testfile", O_RDWR);
	if(!fd)
		assert(0);
	switch (id)
	{
		case 0:{
			off_th = thread0_off;
			break;	
		}
		case 1:{
			off_th = thread1_off;
			break;
		}
		case 2:{
			off_th = thread2_off;
			break;
		}
		
	default:
		assert(0);
		break;
	}
	if(!lseek(fd, off_th, SEEK_SET))
		exit(-1);

	buf = new char[(4 << 20)];

	for (unsigned long i = 0; i < BUF_SIZE; i++) 
		buf[i] = '1' + id;
	/*
	for(auto it : io_list)
		cout << it << endl;
	*/
}

void io_bench::do_write(void)
{
	int bytes_written;
	unsigned long random_range;
	uint32_t count = 0;

	random_range = file_size_bytes / io_size;

    pthread_barrier_wait(&tsync);

	if (1) {
		time_stats_init(&stats, 1);
		time_stats_start(&stats);
	}

	//cout << "# of ops: " << ops_cap << endl;

	if (test_type == SEQ_WRITE || test_type == SEQ_WRITE_READ) {
		unsigned int _io_size = io_size;
		for (unsigned long i = 0; i < file_size_bytes; i += io_size) {
			count++;
			if (i + io_size > file_size_bytes)
				_io_size = file_size_bytes - i;
			else
				_io_size = io_size;

			bytes_written = write(fd, buf, _io_size);

#if 0
			if (do_fsync) {
				fsync(fd);
			}
#endif

			if (bytes_written != io_size) {
				printf("write request %u received len %d\n",
						_io_size, bytes_written);
				errx(1, "write");
			}
			if(count >= ops_cap)
				break;
		}
	} else if (test_type == RAND_WRITE || test_type == ZIPF_WRITE) {
		unsigned int _io_size = io_size;
		for (auto it : io_list) {
			count++;
			/*
			if (it + io_size > file_size_bytes) {
				_io_size = file_size_bytes - it;
				cout << _io_size << endl;
			} else
				_io_size = io_size;
			*/

			lseek(fd, it, SEEK_SET);
			bytes_written = write(fd, buf, _io_size);
			if (bytes_written != _io_size) {
				printf("write request %u received len %d\n",
						_io_size, bytes_written);
				errx(1, "write");
			}
			if(count >= ops_cap)
				break;
		}
	}
    else if (test_type == ZIPF_MIX) {
		unsigned int _io_size = io_size;
        std::list<uint8_t>::iterator op_it = op_list.begin();
		for (auto it : io_list) {
			count++;
			lseek(fd, it, SEEK_SET);

            //read
            if (*op_it == 0) {
                read(fd, buf, io_size);
            }
            //write
            else {
                bytes_written = write(fd, buf, _io_size);
    			if (bytes_written != _io_size) {
    				printf("write request %u received len %d\n",
    						_io_size, bytes_written);
    				errx(1, "write");
    			}
            }	
		    ++op_it;
		    if(count >= ops_cap)
		    	break;
		}
    }
	
	if (do_fsync) {
		fsync(fd);
	}
	
	if (1) {
		time_stats_stop(&stats);

		printf("%s throughput: %3.3f MB ,avg_time %3.3fms ,tid: %d, %d\n", io_bench::get_test_str(test_type).c_str(),
                (float)(file_size_bytes) / (1024.0 * 1024.0 * (float) time_stats_get_avg(&stats)),
				(float)time_stats_get_avg(&stats), id ,gettid());

	}
	
	return ;
}

void io_bench::do_read(void)
{
	int ret;
	uint32_t count = 0;
	struct stat stat;
        pthread_barrier_wait(&tsync);
	fstat(fd, &stat);
	printf("filesize %d", stat.st_size);
	if (per_thread_stats) {
		time_stats_init(&stats, 1);
		time_stats_start(&stats);
	}

	//cout << "# of ops: " << ops_cap << endl;

	if (test_type == SEQ_READ || test_type == SEQ_WRITE_READ) {
		for (unsigned long i = 0; i < file_size_bytes ; i += io_size) {
			count++;
			if (i + io_size > file_size_bytes)
				io_size = file_size_bytes - i;
			else
				io_size = io_size;
#ifdef VERIFY
			memset(buf, 0, io_size);

#endif
			ret = read(fd, buf, io_size);
#if 0
			if (ret != io_size) {
				printf("read size mismatch: return %d, request %lu\n",
						ret, io_size);
			}
#endif
#ifdef VERIFY
			// verify buffer
			for (int j = 0; j < io_size; j++) {
				if (buf[j] != '0' + ((i+(unsigned long)id) % 10)) {
					//hexdump(buf + j, 256);
					printf("read data mismatch at %lu\n", i);
					printf("expected %c read %c\n", (int)('0' + (i % 10)), buf[j]);
					//exit(-1);
					break;
				}
			}
#endif
			if(count >= ops_cap)
				break;
		}
	} else if (test_type == RAND_READ || test_type == ZIPF_READ) {
		for (auto it : io_list) {
			count++;
		/*
			if (it + io_size > file_size_bytes)
				io_size = file_size_bytes - it;
			else
				io_size = io_size;
		*/

			ret = pread(fd, buf, io_size, it);
			if(count >= ops_cap)
				break;
		}
	}

#if 0
	for (unsigned long i = 0; i < file_size_bytes; i++) {
		int bytes_read = read(fd, buf+i, io_size + 100);

		if (bytes_read != io_size) {
			printf("read too far: length %d\n", bytes_read);
		}
	}
#endif

	if (per_thread_stats)  {
		time_stats_stop(&stats);
		//time_stats_print(&stats, (char *)"---------------");

		//printf("%f\n", (float) time_stats_get_avg(&stats));

		printf("%s throughput: %3.3f MB\n", io_bench::get_test_str(test_type).c_str(),
                (float)(file_size_bytes) / (1024.0 * 1024.0 * (float) time_stats_get_avg(&stats)));
	}

	return ;
}

void io_bench::Run(void)
{
	cout << "thread " << id << " start - " << endl;
	cout << "file: " << fd << endl;

	if (test_type == SEQ_READ || test_type == RAND_READ || test_type == ZIPF_READ)
		this->do_read();
	else {
		this->do_write();
	}

	if (test_type == SEQ_WRITE_READ)
		this->do_read();
	
	//pthread_mutex_lock(&cv_mutex);
	//pthread_cond_signal(&cv);
	pthread_mutex_unlock(&cv_mutex);

	return;
}

void io_bench::cleanup(void)
{
	close(fd);

#if 0
	if (test_type == SEQ_READ || test_type == RAND_READ) {
		// Read data integrity check.
		for (unsigned long i = 0; i < file_size_bytes; i++) {
			if (buf[i] != '0' + (i % 10)) {
				hexdump(buf + i, 256);
				printf("read data mismatch at %lu\n", i);
				printf("expected %c read %c\n", (int)('0' + (i % 10)), buf[i]);
				exit(-1);
			}
		}

		printf("Read data matches\n");
	}
#endif

#ifdef ODIRECT
	free(buf);
#else
	
	delete buf;
	
#endif
}

unsigned long io_bench::str_to_size(char* str)
{
	/* magnitude is last character of size */
	char size_magnitude = str[strlen(str)-1];
	/* erase magnitude char */
	str[strlen(str)-1] = 0;
	unsigned long file_size_bytes = strtoull(str, NULL, 0);
	switch(size_magnitude) {
		case 'g':
		case 'G':
			file_size_bytes *= 1024;
		case 'm':
		case 'M':
			file_size_bytes *= 1024;
		case '\0':
		case 'k':
		case 'K':
			file_size_bytes *= 1024;
			break;
		case 'p':
		case 'P':
			file_size_bytes *= 4;
			break;
		case 'b':
		case 'B':
         break;
		default:
			std::cout << "incorrect size format " << str << endl;
			break;
	}
	return file_size_bytes;
}

string io_bench::get_test_str(test_t test)
{
	if (test == SEQ_READ) {
		return "SEQ_READ";
	}
	else if (test == SEQ_WRITE) {
		return "SEQ_WRITE";
	}
	else if (test == SEQ_WRITE_READ) {
		return "SEQ_WRITE_READ";
	}
	else if (test == RAND_WRITE) {
		return "RAND_WRITE";
	}
	else if (test == RAND_READ) {
		return "RAND_READ";
	}
	else if (test == ZIPF_WRITE) {
		return "ZIPF_WRITE";
	}
	else if (test == ZIPF_READ) {
		return "ZIPF_READ";
	}
    else if (test == ZIPF_MIX) {
        return "ZIPF_MIX";
    }
}

test_t io_bench::get_test_type(char *test_type)
{
	/**
	 * Check the mode to bench: read or write and type
	 */
	if (!strcmp(test_type, "sr")){
		return SEQ_READ;
	}
	else if (!strcmp(test_type, "sw")) {
		return SEQ_WRITE;
	}
	else if (!strcmp(test_type, "wr")) {
		return SEQ_WRITE_READ;
	}
	else if (!strcmp(test_type, "rw")) {
		return RAND_WRITE;
	}
	else if (!strcmp(test_type, "rr")) {
		return RAND_READ;
	}
	else if (!strcmp(test_type, "zw")) {
		return ZIPF_WRITE;
	}
	else if (!strcmp(test_type, "zr")) {
		return ZIPF_READ;
	}
    else if (!strcmp(test_type, "zm")) {
        return ZIPF_MIX;
    }
	else { 
		show_usage("iobench");
		cerr << "unsupported test type" << test_type << endl;
		exit(-1);
	}
}

#define HEXDUMP_COLS 8
void io_bench::hexdump(void *mem, unsigned int len)
{
	unsigned int i, j;

	for(i = 0; i < len + ((len % HEXDUMP_COLS) ?
				(HEXDUMP_COLS - len % HEXDUMP_COLS) : 0); i++) {
		/* print offset */
		if(i % HEXDUMP_COLS == 0) {
			printf("0x%06x: ", i);
		}

		/* print hex data */
		if(i < len) {
			printf("%02x ", 0xFF & ((char*)mem)[i]);
		} else {/* end of block, just aligning for ASCII dump */
			printf("	");
		}

		/* print ASCII dump */
		if(i % HEXDUMP_COLS == (HEXDUMP_COLS - 1)) {
			for(j = i - (HEXDUMP_COLS - 1); j <= i; j++) {
				if(j >= len) { /* end of block, not really printing */
					printf(" ");
				} else if(isprint(((char*)mem)[j])) { /* printable char */
					printf("%c",(0xFF & ((char*)mem)[j]));
				} else {/* other char */
					printf(".");
				}
			}
			printf("\n");
		}
	}
}

void io_bench::show_usage(const char *prog)
{
	std::cerr << "usage: " << prog
		<< " [-d <directory>] [-f <file-prefix>] [-n <# of ops>] [-s 'fsync'] [-p 'process sync'] <sr/sw/rr/rw/zr/zw/zm/wr>"
		<< " <size: X{G,M,K,P}, eg: 100M> <IO size, e.g.: 4K> <# of thread>\n"
      << endl;

	std::cerr << "*workload types*\n"
		<< " sr : sequential read\n"
		<< " sw : sequential write\n"
		<< " rr : random read\n"
		<< " rw : random write\n"
		<< " zr : zipfian read\n"
		<< " zw : zipfian write\n"
		<< " zm : zipfian mixed\n"
		<< " wr : sequential write->read\n"
      << endl;
}

/* Returns new argc */
static int adjust_args(int i, char *argv[], int argc, unsigned del)
{
   if (i >= 0) {
      for (int j = i + del; j < argc; j++, i++)
         argv[i] = argv[j];
      argv[i] = NULL;
      return argc - del;
   }
   return argc;
}

#if 0
int process_opt_args(int argc, char *argv[])
{
   int dash_d = -1;

   for (int i = 0; i < argc; i++) {
      if (strncmp("-d", argv[i], 2) == 0) {
         test_dir_prefix = argv[i+1];
         dash_d = i;
      }
   }

   return adjust_args(dash_d, argv, argc, 2);
}
#endif

int process_opt_args(int argc, char *argv[])
{
   int dash_d = -1;
restart:
   for (int i = 0; i < argc; i++) {
      //printf("argv[%d] = %s\n", i, argv[i]);
      if (strncmp("-d", argv[i], 2) == 0) {
         test_dir_prefix = argv[i+1];
         dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 2);
	 goto restart;
      }
      else if (strncmp("-n", argv[i], 2) == 0) {
	 ops_cap = strtoull(argv[i+1], NULL, 0);
         dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 2);
	 goto restart;
      }
      else if (strncmp("-f", argv[i], 2) == 0) {
	 test_file_name = argv[i+1];
         dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 1);
	 goto restart;
      }
      else if (strncmp("-s", argv[i], 2) == 0) {
	 do_fsync = 1;
         dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 1);
	 goto restart;
      }
      else if (strncmp("-p", argv[i], 2) == 0) {
	 psync = 1;
         dash_d = i;
	 argc = adjust_args(dash_d, argv, argc, 1);
	 goto restart;
      }
   }

   return argc;
}

int main(int argc, char *argv[])
{
	int n_threads, i;
	std::vector<io_bench *> io_workers;
	unsigned long file_size_bytes;
	unsigned int io_size = 0;
	struct time_stats main_stats, total_stats;
	const char *device_id;
	// shm is for estimating multiprocess working. not used for now.
	// int *shm_proc;
	// int fd, res;
	char *read_buf;
	read_buf = new char[4096];
		
	int fd;
	fd = open("/mlfs/test_file", O_RDWR | O_CREAT,
		S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
        
	char zipf_file_name[100];
	test_t cur_test;

	int n_repeat = 1;

	device_id = getenv("FILE_ID");
	ops_cap = 0;
	do_fsync = 0;
	psync = 0;

	if (device_id) 
		dev_id = atoi(device_id);
	else
		dev_id = 0;

	// shm_proc = (int*)create_shm(fd, res);
	
	argc = process_opt_args(argc, argv);

	if (argc < 5) {
		if(psync) {
			printf("Setting shm_proc to zero\n");
			// *shm_proc = 0;
		}
		else {
			io_bench::show_usage(argv[0]);
		}
		exit(-1);
	}

	n_threads = std::stoi(argv[4]);

	file_size_bytes = io_bench::str_to_size(argv[2]);
	io_size = io_bench::str_to_size(argv[3]);

	if (io_bench::get_test_type(argv[1]) == ZIPF_WRITE ||
			io_bench::get_test_type(argv[1]) == ZIPF_READ
            || io_bench::get_test_type(argv[1]) == ZIPF_MIX) {
		if (argc != 6) {
			cout << "must supply zipf file" << endl;
			exit(-1);
		}

		strncpy(zipf_file_name, argv[5], 100);
	} else 
		strncpy(zipf_file_name, "none", 4); 

	//std::cout << "Total file size: " << file_size_bytes << "B" << endl
	//	<< "io size: " << io_size << "B" << endl
	//	<< "# of thread: " << n_threads << endl;

	//if(do_fsync)
	//	std::cout << "Sync mode" << endl;

	if(!ops_cap)
		ops_cap = file_size_bytes / io_size;
	else
		ops_cap = min(file_size_bytes / io_size, ops_cap);

	cur_test = io_bench::get_test_type(argv[1]);
	
begin:
	if(cur_test == SEQ_WRITE_READ)
		cur_test = SEQ_WRITE;
	
	io_workers.clear();

	for (i = 0; i < n_threads; i++) {
		io_workers.push_back(new io_bench(i, 
					file_size_bytes,
					io_size,
					cur_test,
					zipf_file_name
					));
	}

        pthread_barrier_init(&tsync, NULL, n_threads);

    if(!psync)
	    time_stats_init(&main_stats, 1);
	//time_stats_init(&total_stats, 1);

	//time_stats_start(&total_stats);

	for (auto it : io_workers) {
		it->prepare();
		pthread_mutex_lock(&it->cv_mutex);
        if(psync)
		    it->per_thread_stats = 1;
        else
            it->per_thread_stats = 0;
	}

	//printf("Waiting for start signal\n");

    // if(psync) {
	//     *shm_proc += 1;
	//     while (*shm_proc > 0){
	// 	    usleep(100);
	//     }
    // }

	printf("Starting benchmark ...\n");

    if(!psync)
	    time_stats_start(&main_stats);
	
	for (auto it : io_workers) {
		it->Start();
		// pthread_mutex_lock(&it->cv_mutex);
	}

	// have been commented
	// for (auto it : io_workers) 
	// 	pthread_cond_wait(&it->cv, &it->cv_mutex); 
	// have been commented
	
	for (auto it : io_workers)
		pthread_mutex_lock(&it->cv_mutex); // where over 3threads get error here
		
	
	for (auto it : io_workers) 
		it->cleanup();
	
	for (auto it : io_workers) 
		it->Join();
	
   

	/*
	if(n_repeat > 0) {
		n_repeat--;
		goto begin;
	}
	*/
	lseek(fd, 0 ,SEEK_SET);
	for (unsigned long i = 0; i < thread1_off; i += 4096UL) {
		if(read(fd, read_buf, 4096UL) < 0)
			exit(-1);
		for(int j=0; j < 4096; j++){
			if(read_buf[j] != '1' + (int)0){
				printf("failed_1\n");
				exit(-1);
			}
		}
	}
	for (unsigned long i = 0; i < thread1_off; i += 4096UL) {
		if(read(fd, read_buf, 4096UL) < 0)
			exit(-1);
		for(int j=0; j < 4096; j++){
			if(read_buf[j] != '1' + (int)1){
				printf("failed_2\n");
				exit(-1);
			}
		}
	}
	for (unsigned long i = 0; i < thread1_off; i += 4096UL) {
		if(read(fd, read_buf, 4096UL) < 0)
			exit(-1);
		for(int j=0; j < 4096; j++){
			if(read_buf[j] != '1' + (int)2){
				printf("failed_3\n");
				exit(-1);
			}
		}
	}

	//printf("--------------------------------------------\n");

	//time_stats_print(&total_stats, (char *)"----------- total stats");
	close(fd);
	delete(read_buf);
	fflush(stdout);
	fflush(stderr);

	return 0;
}
