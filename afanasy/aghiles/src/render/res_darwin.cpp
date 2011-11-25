#ifdef MACOSX
#include "res.h"

#include <fcntl.h>
#include <mach/host_info.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/sysctl.h>

/* All this IOKit stuff is for the disk drives statistics. */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/IOBSD.h>


/* WARNING: from gataddrs man pages on Mac Os X
   If both <net/if.h> and <ifaddrs.h> are being included, <net/if.h> must be
   included before <ifaddrs.h>.
*/
#include <net/if.h>
#include <ifaddrs.h>

#include "../libafanasy/environment.h"

#define AFOUTPUT
#undef AFOUTPUT
#include "../include/macrooutput.h"

host_cpu_load_info_data_t cpuload;


vm_statistics_data_t vm_stats;
unsigned int maxmem;
int pageshift  =  0;
int pagesize   =  0;
int swappgsin  = -1;
int swappgsout = -1;

struct res
{
   int user;
   int nice;
   int system;
   int idle;
   int iowait;
   int irq;
   int softirq;
} r0,r1;

struct net
{
   int recv;
   int send;
} network_statistics[2] = { {0,0}, {0,0} };

/* most drives we will record */
#define MAXDRIVES 16

/* largest drive name we allow */
#define MAXDRIVENAME 31

static mach_port_t s_master_port;

static unsigned s_got_drive_stats = 0;
static unsigned s_num_drives = 0;
static struct timeval cur_time, last_time;

/* Statistics from one disk drive. */
struct drivestats
{
   io_registry_entry_t driver;

   char name[MAXDRIVENAME + 1];

   u_int64_t blocksize;
   u_int64_t total_read_bytes;
   u_int64_t total_write_bytes;
   u_int64_t total_transfers;
   u_int64_t total_time;

} s_drivestats[MAXDRIVES] = {0};

/* declarations, this is taken from iostat.h (DARWIN) . */
static int record_all_devices(
   mach_port_t i_master_port, drivestats *o_drivestats );
static void get_drive_stats(long double etime, double &, double &);
static double compute_etime( struct timeval cur_time, struct timeval prev_time);

unsigned int memtotal, memfree, membuffers, memcached, swaptotal, swapfree;

int now = 0;

void GetResources( af::Host & host, af::HostRes & hres, bool getConstants, bool verbose)
{
   //
   // CPU info:
   //
   if( getConstants)
   {
      // number of pocessors
      host.cpu_num = sysconf(_SC_NPROCESSORS_ONLN);

      // frequency of first pocessor
      uint64_t hz;
      size_t size = sizeof(hz);
      if( sysctlbyname("hw.cpufrequency", &hz, &size, NULL, 0) == 0)
         host.cpu_mhz = hz >> 20;
      else 
         perror( "sysctlbyname" ); 
   }

   //
   // Memory & Swap total and usage:
   //
   {
      if( getConstants )
      {
         size_t size = sizeof(maxmem);
         sysctlbyname("hw.physmem", &maxmem, &size, NULL, 0);
         pagesize = getpagesize();

         memtotal = maxmem >> 10;
         printf("Memory Total = %u KBytes\n", memtotal);
         printf("Memory Page Size = %u Bytes\n", pagesize);

         pageshift = 0;
         while(( pagesize >>= 1) > 0) pageshift++;
         pageshift -= 10;
         printf("Memory Page Shift = %d\n", pageshift);
      }

#define pagetok(size) ((size)<<pageshift)

      unsigned int count = HOST_VM_INFO_COUNT;
      if( host_statistics( mach_host_self(), HOST_VM_INFO, (host_info_t)&vm_stats, &count) == KERN_SUCCESS)
      {
         memfree    = pagetok( vm_stats.free_count);
         membuffers = 0;
         memcached  = pagetok( vm_stats.inactive_count);
         if( swappgsin < 0)
         {
            swapfree = 0;
         }
         else
         {
            int swapin  = pagetok( vm_stats.pageins  - swappgsin );
            int swapout = pagetok( vm_stats.pageouts - swappgsout);
            swapfree = (swapin + swapout);// >> 10;
         }

         swappgsin  = vm_stats.pageins;
         swappgsout = vm_stats.pageouts;
      }
      else
      {
         AFERRPE("GetResources: host_satistics( HOST_VM_INFO) failure:");
      }

      if( getConstants )
      {
         host.mem_mb  = memtotal >> 10;
         host.swap_mb = 0;
      }
      hres.mem_free_mb    = ( memfree + memcached + membuffers ) >> 10;
      hres.mem_cached_mb  = memcached  >> 10;
      hres.mem_buffers_mb = membuffers >> 10;
      hres.swap_used_mb   = swapfree / af::Environment::getRenderUpdateSec();
   }//memory

   //
   // CPU usage:
   //
   {
      res * rl = &r0; res * rn = &r1;
      if( now ) {     rl = &r1;       rn = &r0; }

      unsigned int count = HOST_CPU_LOAD_INFO_COUNT;

      if( host_statistics( mach_host_self(), HOST_CPU_LOAD_INFO, (host_info_t)&cpuload, &count) == KERN_SUCCESS)
      {
         rn->user   = (unsigned long) (cpuload.cpu_ticks[CPU_STATE_USER]);
         rn->nice   = (unsigned long) (cpuload.cpu_ticks[CPU_STATE_NICE]);
         rn->system = (unsigned long) (cpuload.cpu_ticks[CPU_STATE_SYSTEM]);
         rn->idle   = (unsigned long) (cpuload.cpu_ticks[CPU_STATE_IDLE]);

         int user    = rl->user     - rn->user;
         int nice    = rl->nice     - rn->nice;
         int system  = rl->system   - rn->system;
         int idle    = rl->idle     - rn->idle;
         int iowait  = rl->iowait   - rn->iowait;
         int irq     = rl->irq      - rn->irq;
         int softirq = rl->softirq  - rn->softirq;
         int total = user + nice + system + idle + iowait + irq + softirq;
         if( total == 0 ) total = 1;

         hres.cpu_user    = ( 100 * user    ) / total;
         hres.cpu_nice    = ( 100 * nice    ) / total;
         hres.cpu_system  = ( 100 * system  ) / total;
         hres.cpu_idle    = ( 100 * idle    ) / total;
         hres.cpu_iowait  = ( 100 * iowait  ) / total;
         hres.cpu_irq     = ( 100 * irq     ) / total;
         hres.cpu_softirq = ( 100 * softirq ) / total;
      }
      else
      {
         AFERRPE("GetResources: host_satistics( HOST_CPU_LOAD_INFO) failure:");
      }

      // CPU Load Average:
      double loadavg[3] = { 0, 0, 0 };
      int nelem = getloadavg( loadavg, 3);
      if( nelem < 2 ) loadavg[1] = loadavg[0];
      if( nelem < 3 ) loadavg[2] = loadavg[1];
      if( loadavg[0] > 25.0 ) loadavg[0] = 25.0;
      if( loadavg[1] > 25.0 ) loadavg[1] = 25.0;
      if( loadavg[2] > 25.0 ) loadavg[2] = 25.0;
      hres.cpu_loadavg1 = 10.0 * loadavg[0];
      hres.cpu_loadavg2 = 10.0 * loadavg[1];
      hres.cpu_loadavg3 = 10.0 * loadavg[2];
   } //cpu

   //
   // HDD space:
   //
   {
      static char path[4096];
      if( getConstants )
      {
         sprintf( path, "%s", af::Environment::getRenderHDDSpacePath().c_str());
         printf("HDD Space Path = '%s'\n", path);
      }
      struct statfs fsd;
      if( statfs( path, &fsd) < 0)
      {
         perror( "fs status:");
      }
      else
      {
         if( getConstants) host.hdd_gb = ((fsd.f_blocks >> 10) * fsd.f_bsize) >> 20;
         hres.hdd_free_gb  = ((fsd.f_bfree  >> 10) * fsd.f_bsize) >> 20;
      }
   }//hdd

   /*
      Network.

      We sum the statistics of all AF_LINK interfaces.
   */

   hres.net_recv_kbsec = 0;
   hres.net_send_kbsec = 0;

   {
      net *nl = &network_statistics[now];
      net *nn = &network_statistics[1-now];

      struct ifaddrs *addr;
      if( getifaddrs( &addr ) != 0 )
         return;

      unsigned net_recv = 0,  net_send = 0;

      for( struct ifaddrs *it = addr; it; it = it->ifa_next )
      {
         /* We're looking for a link level address. */
         if( !it->ifa_addr || it->ifa_addr->sa_family != AF_LINK )
            continue;

         /* This should not happen but there are some pretty wild stuff
            out there so better be careful. */
         if( !it->ifa_name || !it->ifa_data )
            continue;

         /* ifa_data holds statistics for AF_LINK interfaces. */
         struct if_data *stats = (struct if_data *) it->ifa_data;

         net_recv += stats->ifi_ibytes;
         net_send += stats->ifi_obytes;
      }

      nn->recv = net_recv;
      nn->send = net_send;
      net_recv = nn->recv - nl->recv;
      net_send = nn->send - nl->send;

      if( net_recv >= 0 )
      {
         hres.net_recv_kbsec = net_recv / af::Environment::getRenderUpdateSec();
         hres.net_recv_kbsec /= 1024;
      }

      if( net_send >= 0 )
      {
         hres.net_send_kbsec = net_send / af::Environment::getRenderUpdateSec();
         hres.net_send_kbsec /= 1024;
      }

      now = 1 - now;
   }

   if( !s_got_drive_stats )
   {
      /*
       * This is the first time we are called.
       * Set the busy time to the system boot time, so the stats are
       * calculated since system boot.
       */
      size_t sizeof_cur_time = sizeof(cur_time);
      if (sysctlbyname("kern.boottime", &cur_time, &sizeof_cur_time, NULL, 0) == -1) 
      {
         perror( "sysctlbyname(\"kern.bootime\",...) failed: " );
         return;
      }

      s_got_drive_stats = 1;
   }

	last_time = cur_time;
	gettimeofday(&cur_time, NULL);
	double etime = compute_etime(cur_time, last_time);

   {
      mach_port_t master_port;

      /*
       * Get the I/O Kit communication handle.
       */
      IOMasterPort(bootstrap_port, &master_port);

      s_num_drives = 
         record_all_devices( master_port, &s_drivestats[0] );

      double read_kb_per_second, write_kb_per_second;
      get_drive_stats( etime, read_kb_per_second, write_kb_per_second );  

      hres.hdd_rd_kbsec = read_kb_per_second;
      hres.hdd_wr_kbsec = write_kb_per_second;
   }
}

/*
	All the followin functions were taken from iostat.c on Darwin. 
	They were modified so not to exit when there is an error.
*/

/*
 * Determine whether an IORegistryEntry refers to a valid
 * I/O device, and if so, record it.
 */
static int
record_device(io_registry_entry_t drive, drivestats *o_drive_stats )
{
	io_registry_entry_t parent;

	CFDictionaryRef properties;
	CFStringRef name;
	CFNumberRef number;

	kern_return_t status;
	
	/* get drive's parent */
	status = IORegistryEntryGetParentEntry(drive, kIOServicePlane, &parent);

	if (status != KERN_SUCCESS)
		return 1;

	if (IOObjectConformsTo(parent, "IOBlockStorageDriver"))
	{
		o_drive_stats->driver = parent;

		/* get drive properties */
		status = IORegistryEntryCreateCFProperties(drive,
			(CFMutableDictionaryRef *)&properties,
			kCFAllocatorDefault,
			kNilOptions);

		if (status != KERN_SUCCESS)
			return 1;

		/* get name from properties */
		name = (CFStringRef)CFDictionaryGetValue(properties,
			CFSTR(kIOBSDNameKey));

		CFStringGetCString(name, o_drive_stats->name, 
			MAXDRIVENAME, CFStringGetSystemEncoding());

		/* get blocksize from properties */
		number = (CFNumberRef)CFDictionaryGetValue(
			properties, CFSTR(kIOMediaPreferredBlockSizeKey));

		CFNumberGetValue(
			number, kCFNumberSInt64Type, &(o_drive_stats->blocksize) );

		/* clean up, return success */
		CFRelease(properties);
		return 0;
	}

	/* failed, don't keep parent */
	IOObjectRelease(parent);
	return 1;
}

/*
 * Record all "whole" IOMedia objects as being interesting.
 */
static int record_all_devices(
   mach_port_t i_master_port, drivestats *o_drivestats )
{
	io_iterator_t drivelist;
	kern_return_t status;

	/*
	 * Get an iterator for IOMedia objects.
	 */
	CFMutableDictionaryRef match = IOServiceMatching("IOMedia");
	CFDictionaryAddValue( match, CFSTR(kIOMediaWholeKey), kCFBooleanTrue );

	status = IOServiceGetMatchingServices(i_master_port, match, &drivelist);

	if (status != KERN_SUCCESS)
		return 1;

	/*
	 * Scan all of the IOMedia objects, and for each
	 * object that has a parent IOBlockStorageDriver, save
	 * the object's name and the parent (from which we can
	 * fetch statistics).
	 *
	 * XXX What about RAID devices?
	 */
	int ndrives = 0;

	io_registry_entry_t drive;

	while ((drive = IOIteratorNext(drivelist)) && ndrives<MAXDRIVES )
	{
		if (!record_device(drive, o_drivestats + ndrives))
		{
			ndrives++;
		}

		IOObjectRelease(drive);
	}
	IOObjectRelease(drivelist);

	return ndrives;
}

static void get_drive_stats(
   long double etime, 
   double &o_read_kb_per_sec,
   double &o_write_kb_per_sec )
{
   CFNumberRef number;
   CFDictionaryRef properties;
   CFDictionaryRef statistics;

   long double read_kb_per_second = 0;
   long double write_kb_per_second = 0;

   u_int64_t value;

   for( unsigned i = 0; i < s_num_drives; i++)
   {
      /*
       * If the drive goes away, we may not get any properties
       * for it.  So take some defaults.
       */
      uint64_t total_read_bytes = 0;
      uint64_t total_write_bytes = 0;
      uint64_t total_transfers = 0;
      uint64_t total_time = 0;

      /* get drive properties */
      kern_return_t status =
         IORegistryEntryCreateCFProperties(
            s_drivestats[i].driver,
            (CFMutableDictionaryRef *)&properties,
            kCFAllocatorDefault,
            kNilOptions);

      if (status != KERN_SUCCESS)
         continue;

      /* get statistics from properties */
      statistics = (CFDictionaryRef)
         CFDictionaryGetValue(
            properties, CFSTR(kIOBlockStorageDriverStatisticsKey));

      if (statistics)
      {
         /*
          * Get I/O volume.
          */
         if ((number = (CFNumberRef)CFDictionaryGetValue(statistics,
                     CFSTR(kIOBlockStorageDriverStatisticsBytesReadKey))))
         {
            CFNumberGetValue(number, kCFNumberSInt64Type, &value);
            total_read_bytes += value;
         }

         if ((number = (CFNumberRef)CFDictionaryGetValue(statistics,
                     CFSTR(kIOBlockStorageDriverStatisticsBytesWrittenKey))))
         {
            CFNumberGetValue(number, kCFNumberSInt64Type, &value);
            total_write_bytes += value;
         }

         /*
          * Get I/O counts.
          */
         if ((number = (CFNumberRef)CFDictionaryGetValue(statistics,
                     CFSTR(kIOBlockStorageDriverStatisticsReadsKey)))) {
            CFNumberGetValue(number, kCFNumberSInt64Type, &value);
            total_transfers += value;
         }
         if ((number = (CFNumberRef)CFDictionaryGetValue(statistics,
                     CFSTR(kIOBlockStorageDriverStatisticsWritesKey)))) {
            CFNumberGetValue(number, kCFNumberSInt64Type, &value);
            total_transfers += value;
         }

         /*
          * Get I/O time.
          */
         if ((number = (CFNumberRef)CFDictionaryGetValue(statistics,
                     CFSTR(kIOBlockStorageDriverStatisticsLatentReadTimeKey)))) {
            CFNumberGetValue(number, kCFNumberSInt64Type, &value);
            total_time += value;
         }
         if ((number = (CFNumberRef)CFDictionaryGetValue(statistics,
                     CFSTR(kIOBlockStorageDriverStatisticsLatentWriteTimeKey)))) {
            CFNumberGetValue(number, kCFNumberSInt64Type, &value);
            total_time += value;
         }
      }

      CFRelease(properties);

      /*
       * Compute delta values and stats.
       */
      uint64_t interval_read_bytes = total_read_bytes - s_drivestats[i].total_read_bytes;
      uint64_t interval_write_bytes = total_write_bytes - s_drivestats[i].total_write_bytes;
      uint64_t interval_transfers = total_transfers - s_drivestats[i].total_transfers;
      uint64_t interval_time = total_time - s_drivestats[i].total_time;

      read_kb_per_second += (interval_read_bytes / etime) / 1024;
      write_kb_per_second += (interval_write_bytes / etime) / 1024;

      /* update running totals */
      s_drivestats[i].total_read_bytes = total_read_bytes;
      s_drivestats[i].total_write_bytes = total_write_bytes;
      s_drivestats[i].total_transfers = total_transfers;
      s_drivestats[i].total_time = total_time;
#if 0
      long double kb_per_transfer;
      long double transfers_per_second;
      long double interval_mb;
      long double blocks_per_second, ms_per_transaction;
      u_int64_t interval_blocks;
      u_int64_t total_blocks;

      u_int64_t total_bytes = total_read_bytes + total_write_bytes;

      /* Could be needed in the future. */
      
      interval_blocks = interval_bytes / drivestat[i].blocksize;
      total_blocks = total_bytes / drivestat[i].blocksize;

      blocks_per_second = interval_blocks / etime;
      transfers_per_second = interval_transfers / etime;

      kb_per_transfer = (interval_transfers > 0) ?
         ((long double)interval_bytes / interval_transfers) 
         / 1024 : 0;

      /* times are in nanoseconds, convert to milliseconds */
      ms_per_transaction = (interval_transfers > 0) ?
         ((long double)interval_time / interval_transfers) 
         / 1000 : 0;

      if (Kflag)
         total_blocks = total_blocks * drivestat[i].blocksize 
            / 1024;

      if (oflag > 0) {
         int msdig = (ms_per_transaction < 100.0) ? 1 : 0;

         if (Iflag == 0)
            printf("%4.0Lf%4.0Lf%5.*Lf ",
                  blocks_per_second,
                  transfers_per_second,
                  msdig,
                  ms_per_transaction);
         else 
            printf("%4.1qu%4.1qu%5.*Lf ",
                  interval_blocks,
                  interval_transfers,
                  msdig,
                  ms_per_transaction);
      } else {
         if (Iflag == 0)
            printf(" %5.2Lf %3.0Lf %5.2Lf ", 
                  kb_per_transfer,
                  transfers_per_second,
                  mb_per_second);
         else {
            interval_mb = interval_bytes;
            interval_mb /= 1024 * 1024;

            printf(" %5.2Lf %3.1qu %5.2Lf ", 
                  kb_per_transfer,
                  interval_transfers,
                  interval_mb);
         }
      }
#endif

   }

   o_read_kb_per_sec = read_kb_per_second;
   o_write_kb_per_sec = write_kb_per_second;

   return;
}

static double
compute_etime(struct timeval cur_time, struct timeval prev_time)
{
	struct timeval busy_time;
	u_int64_t busy_usec;
	long double etime;

	timersub(&cur_time, &prev_time, &busy_time);

	busy_usec = busy_time.tv_sec;  
	busy_usec *= 1000000;          
	busy_usec += busy_time.tv_usec;
	etime = busy_usec;
	etime /= 1000000;

	return(etime);
}



#endif