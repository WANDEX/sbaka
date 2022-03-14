
/*
 * sbull.h -- definitions for the char module
 *
 * Copyright (C) 2001 Alessandro Rubini and Jonathan Corbet
 * Copyright (C) 2001 O'Reilly & Associates
 *
 * The source code in this file can be freely used, adapted,
 * and redistributed in source or binary form, so long as an
 * acknowledgment appears in derived source files.  The citation
 * should list that the code comes from the book "Linux Device
 * Drivers" by Alessandro Rubini and Jonathan Corbet, published
 * by O'Reilly & Associates.   No warranty is attached;
 * we cannot take responsibility for errors or fitness for use.
 *
 */


#include <linux/ioctl.h>

/*
 * Macros to help debugging
 */

#undef PDEBUG             /* undef it, just in case */
#ifdef SBAKA_DEBUG
#  ifdef __KERNEL__
     /* This one if debugging is on, and kernel space */
#    define PDEBUG(fmt, args...) printk( KERN_DEBUG "sbaka: " fmt, ## args)
#  else
     /* This one for user space */
#    define PDEBUG(fmt, args...) fprintf(stderr, fmt, ## args)
#  endif
#else
#  define PDEBUG(fmt, args...) /* not debugging: nothing */
#endif

#undef PDEBUGG
#define PDEBUGG(fmt, args...) /* nothing: it's a placeholder */


#define SBAKA_MAJOR 0       /* dynamic major by default */
#define SBAKA_DEVS 2        /* two disks */
#define SBAKA_RAHEAD 2      /* two sectors */
#define SBAKA_SIZE 2048     /* two megs each */
#define SBAKA_BLKSIZE 1024  /* 1k blocks */
#define SBAKA_HARDSECT 512  /* 2.2 and 2.4 can used different values */

#define SBAKAR_MAJOR 0      /* Dynamic major for raw device */
