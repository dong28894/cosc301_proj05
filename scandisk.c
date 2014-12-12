#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <string.h>
#include <stdbool.h>

#include "bootsect.h"
#include "bpb.h"
#include "direntry.h"
#include "fat.h"
#include "dos.h"

void write_dirent(struct direntry *dirent, char *filename, 
		  uint16_t start_cluster, bool *refered_sectors,
			uint8_t *image_buf, struct bpb33* bpb){
	int size = 0;
    /* clean out anything old that used to be here */
    memset(dirent, 0, sizeof(struct direntry));    

    /* set the file name and extension */
    memset(dirent->deName, ' ', 8);
    memcpy(dirent->deExtension, "dat", 3);    

    if (strlen(filename)>8){
		filename[8]='\0';
    }
    memcpy(dirent->deName, filename, strlen(filename));

    /* set the attributes and file size */
    dirent->deAttributes = ATTR_NORMAL;
    putushort(dirent->deStartCluster, start_cluster);
	uint16_t cluster = start_cluster;
	while (is_valid_cluster(cluster, bpb)){
		size++;
		refered_sectors[cluster] = true;		
		cluster = get_fat_entry(cluster, image_buf, bpb);		
	}
    putulong(dirent->deFileSize, size*bpb->bpbBytesPerSec);
}


void create_dirent(struct direntry *dirent, char *filename, 
		   uint16_t start_cluster, bool *refered_sectors,
			uint8_t *image_buf, struct bpb33* bpb){
    while (1){
		if (dirent->deName[0] == SLOT_EMPTY){
	    	/* we found an empty slot at the end of the directory */
	    	write_dirent(dirent, filename, start_cluster, refered_sectors, image_buf, bpb);
	    	dirent++;

	    	/* make sure the next dirent is set to be empty, just in
	       case it wasn't before */
	    	memset((uint8_t*)dirent, 0, sizeof(struct direntry));
	    	dirent->deName[0] = SLOT_EMPTY;
	    	return;
		}

		if (dirent->deName[0] == SLOT_DELETED){
	    	/* we found a deleted entry - we can just overwrite it */
	    	write_dirent(dirent, filename, start_cluster, refered_sectors, image_buf, bpb);
	    	return;
		}
		dirent++;
    }
}

void find_orphaned(uint8_t *image_buf, struct bpb33* bpb, bool *refered_sectors){
	int deIndex = 1;
	char filename[15] = "found";
	char buffer[10];
	struct direntry *dirent = (struct direntry*)root_dir_addr(image_buf, bpb);
	bool start_cluster[bpb->bpbSectors]; //a bool array to check if a sector is the start of a chain
	for (int i = 0; i < bpb->bpbSectors; i++){
		start_cluster[i] = true;	
	}
	for (int i = CLUST_FIRST; i < bpb->bpbSectors; i++){
		//If a cluster is refered by some FAT entry, it can't be the start cluster
		uint16_t refered = get_fat_entry(i, image_buf, bpb);
		start_cluster[refered] = false;
	}
	for (int i = CLUST_FIRST; i < bpb->bpbSectors; i++){
		if (refered_sectors[i] == false && get_fat_entry(i, image_buf, bpb) != CLUST_FREE && start_cluster[i] == true){
			//If we didn't visit this cluster while traversing the disk image 
			//but its FAT entry is not marked free and it's not refered by any other FAT entry 
			printf("%d\n", i);
			sprintf(buffer, "%d", deIndex);
			strcat(filename, buffer);
			create_dirent(dirent, filename, i, refered_sectors, image_buf, bpb);
			filename[5] = '\0';
			deIndex++;
		}
	}
}

int check_file_inconsistency(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb, bool *refered_sectors){
	uint32_t size = getulong(dirent->deFileSize);
	uint16_t cluster = getushort(dirent->deStartCluster);
	uint16_t cluster_size = bpb->bpbBytesPerSec * bpb->bpbSecPerClust;
	int numClusters = (size + cluster_size - 1)/cluster_size;
	int realNumClusters = 0;
	while (is_valid_cluster(cluster, bpb)){
		realNumClusters++;
		refered_sectors[cluster] = true;		
		uint16_t next_cluster = get_fat_entry(cluster, image_buf, bpb);
		if (next_cluster != cluster){
			cluster = next_cluster;
		}else{
			set_fat_entry(cluster, FAT12_MASK&CLUST_BAD, image_buf, bpb);
			printf("metadata size: %d\n", numClusters);
			printf("Real size: %d\n", realNumClusters);
			if (numClusters <= realNumClusters){			
				return -1;
			}else if (numClusters > realNumClusters){
				return 1;
			}
		}		
	}
	printf("metadata size: %d\n", numClusters);
	printf("Real size: %d\n", realNumClusters);
	if (numClusters < realNumClusters){			
		return -1;
	}else if (numClusters > realNumClusters){
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

uint16_t check_dirent(struct direntry *dirent, uint8_t *image_buf, struct bpb33* bpb, bool *refered_sectors){
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
			refered_sectors[followclust] = true;
        }
    }
    else{
        /*
         * a "regular" file entry
         */
		printf("%s.%s\n", name, extension);
		if (check_file_inconsistency(dirent, image_buf, bpb, refered_sectors)){
			printf("Inconsistent\n");
			fix_file_inconsistency(dirent, image_buf, bpb);
		}
    }

    return followclust;
}

void follow_dir(uint16_t cluster, uint8_t *image_buf, struct bpb33* bpb, bool *refered_sectors){
    while (is_valid_cluster(cluster, bpb)){
        struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

        int numDirEntries = (bpb->bpbBytesPerSec * bpb->bpbSecPerClust) / sizeof(struct direntry);
        int i = 0;
		for ( ; i < numDirEntries; i++){            
            uint16_t followclust = check_dirent(dirent, image_buf, bpb, refered_sectors);
            if (followclust)
                follow_dir(followclust, image_buf, bpb, refered_sectors);
            dirent++;
		}
		cluster = get_fat_entry(cluster, image_buf, bpb);
    }
}

void traverse_root(uint8_t *image_buf, struct bpb33* bpb, bool *refered_sectors)
{
    uint16_t cluster = 0;

    struct direntry *dirent = (struct direntry*)cluster_to_addr(cluster, image_buf, bpb);

    int i = 0;
    for ( ; i < bpb->bpbRootDirEnts; i++)
    {
        uint16_t followclust = check_dirent(dirent, image_buf, bpb, refered_sectors);
        if (is_valid_cluster(followclust, bpb))
            follow_dir(followclust, image_buf, bpb, refered_sectors);

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
	bool refered_sectors[bpb->bpbSectors];
	for (int i = 0; i < bpb->bpbSectors; i++){
		refered_sectors[i] = false;	
	}
	traverse_root(image_buf, bpb, &refered_sectors[0]);
	find_orphaned(image_buf, bpb, &refered_sectors[0]);



    unmmap_file(image_buf, &fd);
    return 0;
}
