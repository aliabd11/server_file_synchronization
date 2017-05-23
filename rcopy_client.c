#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <libgen.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "ftree.h"
#include "hash.h"
#include "hash_functions.c"


#ifndef PORT
  #define PORT 30000
#endif

struct client {
    int fd;
    struct in_addr ipaddr;
    struct client *next;
};


void file_struct_copy(char *source, char *host, int sock_fd, int transfer);
void transfer_fork(char *source, char *host);

static char *main_directory; // keeps the name of the main directory being passed in
static int error_encountered = 0;

int transfer_file(char *source, char *host, int file_size, int sock_fd);

int main(int argc, char **argv) {
    /* Note: In most cases, you'll want HOST to be localhost or 127.0.0.1, so
     * you can test on your local machine.*/
    //char *dest;
    if (argc != 3) {
        printf("Usage:\n\trcopy_client SRC HOST\n");
        printf("\t SRC - The file or directory to copy to the server\n");
        printf("\t HOST - The hostname of the server");
        return 1;
    }
    main_directory = basename(argv[1]); // setting the name of the main directory
    if (rcopy_client(argv[1], argv[2], PORT) != 0) {
        printf("Errors encountered during copy\n");
        return 1;
    } else {
        printf("Copy completed successfully\n");

        return 0;
    }
}

int rcopy_client(char *source, char *host, unsigned short port) {
    // Create the socket FD.
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("client: socket");
        exit(1);
    }

    // Set the IP and port of the server to connect to.
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &server.sin_addr) < 1) {
        perror("client: inet_pton");
        close(sock_fd);
        exit(1);
    }

    // Connect to the server.
    if (connect(sock_fd, (struct sockaddr *)&server, sizeof(server)) == -1) {
        perror("rcopy_client: client: connect");
        close(sock_fd);
        exit(1);
    }

    file_struct_copy(source, host, sock_fd, 0);

    if(close(sock_fd) == -1) {
        perror("close");
    }

    if (error_encountered == 1) {
      return 1;
    }
    else {
      return(0);
    }
}

void file_struct_copy(char *source, char *host, int sock_fd, int transfer) {
    //Now want to trace through directory tree:
    int each_child_created = 0; //amount of children processes

    char *full_path; // The full file path to pass in
    full_path = malloc(sizeof(char) * 255); // The full file path to pass in

    strncpy(full_path, source, 255);

    //Get file information
    struct stat file_info;
    if (lstat(source, &file_info) != 0) {
        perror("lstat");
        exit(-1);
    }
    if (S_ISREG(file_info.st_mode)) {
        int request_type;
        if (transfer == 0) {
            //Write request type
            request_type = REGFILE;
            if(write(sock_fd, &request_type, sizeof(int)) == -1) {
                perror("write");
            }
        }
        else if (transfer == 1) {//we're transferring a file
            request_type = TRANSFILE;
            if(write(sock_fd, &request_type, sizeof(int)) == -1) {
                perror("write");
                exit(1);
            }
        }

        //Write the path (basename)
        char *base_file_name = strstr(full_path, (const char*)main_directory);
        if(write(sock_fd, base_file_name, MAXPATH) == -1) {
            perror("write");
            exit(1);
        }

        //Write the mode //mode_t is an unsigned int
        int file_mode = file_info.st_mode;
        int net_byte_order_mode = htonl(file_mode);
        if(write(sock_fd, &(net_byte_order_mode), sizeof(int)) == -1) {
            perror("write");
            exit(1);
        }

        //Write the file hash
        FILE *source_file_val;
        source_file_val = fopen(source, "rb");
        if (source_file_val == NULL) {
            fprintf(stderr, "Error opening file\n");
            exit(-1);
        }
        char *hash_val_src = hash(source_file_val);
        if(write(sock_fd, hash_val_src, BLOCKSIZE) == -1) {
            perror("write");
            exit(1);
        }
        if ((fclose(source_file_val)) != 0) {
            fprintf(stderr, "Error: fclose failed\n");
            exit(1);
        }

        //Write file size
        int file_size = file_info.st_size;
        int network_byte_order_size = htonl(file_size);
        if(write(sock_fd, &(network_byte_order_size), sizeof(int)) == -1) {
            perror("write");
            exit(1);
        }

        if (transfer == 0) {
            int server_response;
            if(read(sock_fd, &(server_response), sizeof(int)) == -1) {
                perror("read");
                exit(1);
            }

            if (server_response == ERROR) {
                error_encountered = 1;
            }
            else if (server_response == SENDFILE) {
                int r = fork();
                if (r == 0) {
                    each_child_created++;
                    transfer_fork(source, host);
                    return;
                }
            }
        }
        else { //transfer == 1
            transfer_file(source, host, file_size, sock_fd);
        }
        return;
    }

    if (S_ISDIR(file_info.st_mode)) {
        int request_type;
        if (transfer == 0) {
            //Write request type
            request_type = REGDIR;
            if(write(sock_fd, &request_type, sizeof(int)) == -1) {
                perror("write");
                exit(1);
            }
        }
        //Write the path
        char *base_file_name = strstr(full_path, (const char*)main_directory);
        if(write(sock_fd, base_file_name, MAXPATH) == -1) {
            perror("write");
            exit(1);
        }

        //Writes the mode //mode_t is an unsigned int
        int file_mode = file_info.st_mode;
        int net_byte_order_mode = htonl(file_mode);
        if(write(sock_fd, &(net_byte_order_mode), sizeof(int)) == -1) {
            perror("write");
            exit(1);
        }

        //Write the hash (NULL for directories?)
        char *hash_val_src = "NULLSIZE"; //set to NULL
        if(write(sock_fd, hash_val_src, BLOCKSIZE) == -1) {
            perror("write");
            exit(1);
        }

        //Write file size
        int file_size = file_info.st_size;
        int network_byte_order_size = htonl(file_size);
        if(write(sock_fd, &(network_byte_order_size), sizeof(int)) == -1) {
            perror("write");
            exit(1);
        }

        //Create struct for directory, open it up for reading
        struct dirent *dp = malloc(sizeof(struct dirent)); //NULL;
        DIR *dirp = NULL;

        dirp = opendir(source); // Open the directory for reading.
        if (dirp == NULL) {
            perror("opendir");
            exit(1);
        }

        //if (transfer == 0) {
        int server_response = 0;
        if(read(sock_fd, &(server_response), sizeof(int)) == 0) {
            perror("read"); //!
            exit(1);
        }
        if (server_response == ERROR) {
            error_encountered = 1; //store the fact that we got an error
        }

        while ((dp = readdir(dirp)) != NULL) {
            if (dp->d_name[0] != '.') {
                // path concatenation and recursion
                strncpy(full_path, source, 255);
                strcat(full_path, "/");
                strcat(full_path, dp->d_name);
                full_path[strlen(full_path)] = '\0';
                rcopy_client(full_path, host, PORT);
            }
        }
        return;
    }
    for (int i = 0; i < each_child_created; i++) {
    //Ensure parent processes wait for their children
    pid_t pid;
    int status;
    if ((pid = wait(&status)) == -1) {
        //perror("wait");
        }
    }
    return;
}

int transfer_file(char *source, char *host, int file_size, int sock_fd) {
    char buffer[MAXDATA]; //Perform actual file copying
    size_t bytes;

    //Open file
    FILE *file_source;
    file_source = fopen(source, "rb");
    if (file_source == NULL) {
        fprintf(stderr, "Error opening file\n");
        close(sock_fd);
        exit(-1);
    }

    while ((bytes = fread(buffer, 1, 2048, file_source)) != 0) {
        if(write(sock_fd, buffer, bytes) < 0) {
            close(sock_fd);
            return 1; //Error encountered, return 1
        }
    }

    if (fclose(file_source) != 0) {
        fprintf(stderr, "Error: fclose failed\n");
        exit(1);
    }
    return 0;

}

void transfer_fork(char *source, char *host) {
    int sock_fd_transf = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_transf < 0) {
        perror("client: socket");
        exit(1);
    }

    // Set the IP and port of the server to connect to.
    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(PORT);
    if (inet_pton(AF_INET, host, &server.sin_addr) < 1) {
        perror("client: inet_pton");
        close(sock_fd_transf);
        exit(1);
    }

    // Connect to the server.
    if (connect(sock_fd_transf, (struct sockaddr *)&server, sizeof(server)) == -1) {
        perror("transfer_fork: client: connect");
        close(sock_fd_transf);
        exit(1);
    }

    //Recurse to send in struct again and transfer file data
    file_struct_copy(source, host, sock_fd_transf, 1);

    return;
}
