#include <stdio.h>
#include <unistd.h>
#include <libgen.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <dirent.h>

#include "ftree.h"
#include "hash.h"
#include "hash_functions.c"

#ifndef PORT
  #define PORT 30000
#endif
#define MAX_CONNECTIONS 30
#define MAX_BACKLOG 5

struct client {
    int fd;
    struct request my_file_information;
    int state;
    FILE *my_file;

    int file_read;
    int transfer_state;
    struct in_addr ipaddr;
    struct client *next;
};


static struct client *addclient(struct client *top, int fd, struct in_addr addr);
static struct client *removeclient(struct client *top, int fd);
int handleclient(struct client *p, struct client *top);
int check_server_copy(struct request *file_struct, char* path);
int copy_file(struct request *file_struct, struct client *p, char *path);


int main(int argc, char **argv) {

    if(argc != 2) {
        printf("Usage:\n\t%s rcopy_server PATH_PREFIX\n", argv[0]);
        printf("\t PATH_PREFIX - The absolute path on the server that is used as the path prefix\n");
        printf("\t        for the destination in which to copy files and directories.\n");
        exit(1);
    }
    /* NOTE:  The directory PATH_PREFIX/sandbox/dest will be the directory in
     * which the source files and directories will be copied.  It therefore
     * needs rwx permissions.  The directory PATH_PREFIX/sandbox will have
     * write and execute permissions removed to prevent clients from trying
     * to create files and directories above the dest directory.
     */

    // create the sandbox directory
    char path[MAXPATH];
    strncpy(path, argv[1], MAXPATH);
    strncat(path, "/", MAXPATH - strlen(path) + 1);
    strncat(path, "sandbox", MAXPATH - strlen(path) + 1);

    if(mkdir(path, 0700) == -1){
        if(errno != EEXIST) {
            fprintf(stderr, "couldn't open %s\n", path);
            perror("mkdir");
            exit(1);
        }
    }

    // create the dest directory
    strncat(path, "/", MAXPATH - strlen(path) + 1);
    strncat(path, "dest", MAXPATH - strlen(path) + 1);
    if(mkdir(path, 0700) == -1){
        if(errno != EEXIST) {
            fprintf(stderr, "couldn't open %s\n", path);
            perror("mkdir");
            exit(1);
        }
    }

    // change into the dest directory.
    chdir(path);

    // remove write and access perissions for sandbox
    //0400
    if(chmod("..", 0400) == -1) {
        perror("chmod");
        exit(1);
    }

    /* IMPORTANT: All path operations in rcopy_server must be relative to
     * the current working directory.
     */
    rcopy_server(PORT, path);

    // Should never get here!
    fprintf(stderr, "Server reached exit point.");
    return 1;
}

int bindandlisten(void);

int rcopy_server(unsigned short port, char* file_path) {
    int clientfd, maxfd, nready;
    struct client *p;
    struct client *head = NULL;
    socklen_t len;
    struct sockaddr_in q;
    struct timeval tv;
    fd_set allset;
    fd_set rset;

    int i;


    int listenfd = bindandlisten();
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;

        nready = select(maxfd + 1, &rset, NULL, NULL, &tv);
        if (nready == 0) {
            printf("No response from clients in %ld seconds\n", tv.tv_sec);
            continue;
        }

        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            len = sizeof(q);
            if ((clientfd = accept(listenfd, (struct sockaddr *)&q, &len)) < 0) {
                perror("accept");
                exit(1);
            }
            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            head = addclient(head, clientfd, q.sin_addr);
        }

        for(i = 0; i <= maxfd; i++) {
            if (FD_ISSET(i, &rset)) {
                for (p = head; p != NULL; p = p->next) {
                    if (p->fd == i) {
                        int result = handleclient(p, head);
                        if (result == -1) {
                            int tmp_fd = p->fd;
                            head = removeclient(head, p->fd);
                            FD_CLR(tmp_fd, &allset);
                            close(tmp_fd);
                        }
                        break;
                    }
                }
            }
        }
    }
    return 0;
}

int handleclient(struct client *p, struct client *top) {
    int k = 1;
    struct request *new_file = &(p->my_file_information);

    //Read in type
    if (p->state == AWAITING_TYPE) {
        int file_type = 0;
        k = read(p->fd, &(file_type), sizeof(int));
        if(k == -1) {
            perror("socket read");
        }
        if ((file_type == REGFILE) || (file_type == REGDIR)) {
            new_file->type = file_type;
            p->state = AWAITING_PATH;
            p->transfer_state = 0;
        }
        else {
            new_file->type = file_type;
            p->state = AWAITING_PATH;
            p->transfer_state = 1;
        }
    }

    //Read in path
    if (p->state == AWAITING_PATH) {
        k = read(p->fd, &(new_file->path), MAXPATH);
        if(k == -1) {
            perror("socket read");
        }
        p->state = AWAITING_PERM;
    }

    //Read in mode
    if (p->state == AWAITING_PERM) {
        int file_size_mode = 0;
        k = read(p->fd, &(file_size_mode), sizeof(int));
        if(k == -1) {
            perror("socket read");
        }

        int net_byte_order_mode = ntohl(file_size_mode);
        new_file->mode = net_byte_order_mode;

        p->state = AWAITING_HASH;
    }

    //Read in hash (char hash[BLOCKSIZE])
    if (p->state == AWAITING_HASH) {
        k = read(p->fd, &(new_file->hash), BLOCKSIZE);
        if(k == -1) {
            perror("socket read");
        }
        p->state = AWAITING_SIZE;
    }

    //Read in size of file
    if (p->state == AWAITING_SIZE) {
        int file_size_net = 0;
        k = read(p->fd, &(file_size_net), sizeof(int));

        if(k == -1) {
            perror("socket read");
        }
        int net_byte_order_size = ntohl(file_size_net);
        new_file->size = net_byte_order_size;

        p->state = AWAITING_DATA;
    }

    if (p->state == AWAITING_DATA) {
        if (p->transfer_state == 0) {
            //Check request type given by server (SENDFILE/OK/ERROR)
            int request_type = check_server_copy(new_file, new_file->path);
            write(p->fd, &request_type, sizeof(int));
            p->transfer_state = 1;
        }
        else if (p->transfer_state == 1) {
            copy_file(new_file, p, new_file->path);
            int copy_val = copy_file(new_file, p, new_file->path);
            if (copy_val == 2) { //finished copying
                return -1;
            }
        }
    }
    return 1;
}

 /* bind and listen, abort on error
  * returns FD of listening socket
  */

int bindandlisten(void) {
    struct sockaddr_in r;
    int listenfd;

    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        exit(1);
    }
    int yes = 1;
    if ((setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int))) == -1) {
        perror("setsockopt");
    }
    memset(&r, '\0', sizeof(r));
    r.sin_family = AF_INET;
    r.sin_addr.s_addr = INADDR_ANY;
    r.sin_port = htons(PORT);

    if (bind(listenfd, (struct sockaddr *)&r, sizeof r)) {
        perror("bind");
        exit(1);
    }
    if (listen(listenfd, 5)) {
        perror("listen");
        exit(1);
    }
    return listenfd;
}

static struct client *addclient(struct client *top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));
    if (!p) {
        perror("malloc");
        exit(1);
    }

    p->fd = fd;
    p->ipaddr = addr;
    p->next = top;
    top = p;
    return top;
}

static struct client *removeclient(struct client *top, int fd) {
    struct client **p;

    for (p = &top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                 fd);
    }
    return top;
}

int copy_file(struct request *file_struct, struct client *p, char* path)  {
    if (file_struct->type == TRANSFILE) { //checking if it's a reg file
        FILE *new_file = fopen(path, "ab");
        if (new_file == NULL) {
            perror("server child: fopen");
            exit(1);
        }
        char buffer[2048]; //Buffer for 512 bytes
        size_t bytes;

        bytes = read(p->fd, buffer, 2048); //read 512 at a time, use select
        p->file_read += bytes;

        if(fwrite(buffer, 1, bytes, new_file) != bytes) {
            return 1;
        }

        if (fclose(new_file) != 0) {
            fprintf(stderr, "Error: fclose failed\n");
            exit(1);
        }

        if ((p->file_read) == file_struct->size) {
            if (chmod(path, file_struct->mode) != 0) {
                perror("chmod");
            }
            return 2; //value for successful copying
        }
        return 0;
    }
    return 0;
}

int check_server_copy(struct request *file_struct, char* path) {
    //Returns either OK, SENDFILE or ERROR
    //printf("Checking the server copy\n");
    int file_status = OK; //OK by default

    struct stat file_info;
    if (lstat(path, &file_info) == -1) {
        if (errno == ENOENT) {
            if (file_struct->type == REGDIR) {
                mkdir(path, file_struct->mode); //need to implement error checking
                return file_status;
            }
            else {
                file_status = SENDFILE;
                return file_status;
            }
        }
    }

    //Store file type of path file
    int file_type = 0;
    if (S_ISREG(file_info.st_mode)) {
        file_type = REGFILE;
    }
    else if (S_ISDIR(file_info.st_mode)) {
        file_type = REGDIR;
    }

    //Check if the file type is the same
    //REGFILE 1, REGDIR 2, TRANSFILE 3
    if ((file_struct->type) != file_type) {
        file_status = ERROR;
        fprintf(stderr, "Differing file types\n");
        return file_status;
    }

    //Update the file’s permissions always
    if (chmod(path, file_struct->mode) != 0) {
            perror("chmod");
    }

    //Check file size
    if (S_ISREG(file_info.st_mode)) {
        if ((file_struct->size) != (file_info.st_size)) {
            file_status = SENDFILE;
            return file_status;
        }
    }

    //Checks hash to see if the files are the same;
    if (S_ISREG(file_info.st_mode)) {
        FILE *path_file;
        path_file = fopen(path, "rb");
        if (path_file == NULL) {
            fprintf(stderr, "Error opening file\n");
            exit(-1);
        }
        char *hash_val_src = hash(path_file);

        if (check_hash(hash_val_src, file_struct->hash) != 0) {
           file_status = SENDFILE;
        }

        if (fclose(path_file) != 0) {
            fprintf(stderr, "Checking Hash: Error: fclose failed\n");
            exit(1);
        }
    }
    return file_status;
}