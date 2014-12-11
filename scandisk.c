#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

int check_file_inconsistency(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb){
	uint32_t size = getulong(dirent->deFileSize);
	uint16_t cluster = getushort(dirent->deStartCluster);
	uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
	int numClusters = (size + cluster_size - 1)/cluster_size;
	int realNumClusters = 0;
	while (is_valid_cluster(cluster, bpb)){
		realNumClusters++;		
		cluster = get_fat_entry(cluster, image_buf, bpb);		
	}
	if (numClusters < realNumClusters){
		printf("metadata size: %d\n", numClusters);
		printf("Real size: %d\n", realNumClusters);	
		return -1;
	}else if (numClusters > realNumClusters){
		printf("metadata size: %d\n", numClusters);
		printf("Real size: %d\n", realNumClusters);
		return 1;
	}else{
		return 0;
	}
}


void fix_file_inconsistency(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb){
	uint32_t size = getulong(dirent->deFileSize);
	uint16_t cluster = getushort(dirent->deStartCluster);
	uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
	int numClusters = (size + cluster_size - 1)/cluster_size;
	int realNumClusters = 0;
	while (is_valid_cluster(cluster, bpb)){
		realNumClusters++;
		if (realNumClusters == numClusters){
			uint16_t tmp = get_fat_entry(cluster, image_buf, bpb);
			set_fat_entry(cluster, FAT12_MASK&CLUST_EOFS, image_buf, bpb);
			cluster = tmp;
		}else if (realNumClusters > numClusters){
			uint16_t tmp = get_fat_entry(cluster, image_buf, bpb);
			set_fat_entry(cluster, FAT12_MASK&CLUST_FREE, image_buf, bpb);
			cluster = tmp;
		}else{
			cluster = get_fat_entry(cluster, image_buf, bpb);
		}
	}	
	if (numClusters > realNumClusters){
		putulong(dirent->deFileSize, realNumClusters*cluster_size);
	}
}

uint16_t check_dirent(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb){
    uint16_t followclust = 0;

    int i;
    char name[9];
    char extension[4];
    uint16_t file_cluster;
    name[8] = ' ';
    extension[3] = ' ';
    memcpy(name, &(dirent->deName[0]), 8);
    memcpy(extension, dirent->deExtension, 3);
    if (name[0] == SLOT_EMPTY){
		return followclust;
    }

    /* skip over deleted entries */
    if (((uint8_t)name[0]) == SLOT_DELETED){
		return followclust;
    }

    if (((uint8_t)name[0]) == 0x2E){
		// dot entry ("." or "..")
		// skip it
        return followclust;
    }

    /* names are space padded - remove the spaces */
    for (i = 8; i > 0; i--){
		if (name[i] == ' ') 
	    	name[i] = '\0';
		else 
	    	break;
    }

    /* remove the spaces from extensions */
    for (i = 3; i > 0; i--){
		if (extension[i] == ' ') 
	    	extension[i] = '\0';
		else 
	    	break;
    }

    if ((dirent->deAttributes & ATTR_WIN95LFN) == ATTR_WIN95LFN){
		// ignore any long file name extension entries
		//
		// printf("Win95 long-filename entry seq 0x%0x\n", dirent->deName[0]);
    }else if ((dirent->deAttributes & ATTR_VOLUME) != 0){
		printf("Volume: %s\n", name);
    }else if ((dirent->deAttributes & ATTR_DIRECTORY) != 0){
        // don't deal with hidden directories; MacOS makes these
        // for trash directories and such; just ignore them.
		if ((dirent->deAttributes & ATTR_HIDDEN) != ATTR_HIDDEN){	    
            file_cluster = getushort(dirent->deStartCluster);
            followclust = file_cluster;
        }
    }
    else{
        /*
         * a "regular" file entry
         * print file name if there's an inconsistency
         */
		if (check_file_inconsistency(dirent, image_buf, bpb)){
			printf("%s.%s\n", name, extension);
			fix_file_inconsistency(dirent, image_buf, bpb);
			check_file_inconsistency(dirent, image_buf, bpb);
		}
    }

    return followclust;
}

void follow_dir(uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb){
    while (is_valid_cluster(cluster, bpb)){
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
		for ( ; i < numDirEntries; i++){            
            uint16_t followclust = check_dirent(dirent, image_buf, bpb);
            if (followclust)
                follow_dir(followclust, image_buf, bpb);
            dirent++;
		}
		cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}

void traverse_root(uint8_t *image_buf, struct bpb33* bpb)
{
    uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = check_dirent(dirent, image_buf, bpb);
        if (is_valid_cluster(followclust, bpb))
            follow_dir(followclust, image_buf, bpb);

        dirent++;
    }
}


void usage(char *progname) {
    fprintf(stderr, "usage: %s <imagename>\n", progname);
    exit(1);
}


int main(int argc, char** argv) {
    uint8_t *image_buf;
    int fd;
    struct bpb33* bpb;
    if (argc < 2) {
	usage(argv[0]);
    }

    image_buf = mmap_file(argv[1], &fd);
    bpb = check_bootsector(image_buf);

    // your code should start here...

	traverse_root(image_buf, bpb);




    unmmap_file(image_buf, &fd);
    return 0;
}
