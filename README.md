# server_file_synchronization

## Synopsis

Server and client code to emulate the functionality of rsync, copying from a client to a server. Utilizes sockets, forking, recursion and a variety of other programming concepts to get the job done in addition to basic hashing to check files. Coded in C and can be compiled using the provided make file.

## Tests

Ex. Consider the following directory strucutre

adir:
    bdir:
        file2
    file1

If server is run as "rcopy_server . where dest is a relative or absolute path on the server, then when we run "rcopy_client adir localhost", contents of dest now look like:

sandbox:
    dest:
        adir:
            bdir:
                file2
            file1


## Note

Not guaranteed to be 100% complete, concurrency problems not dealt with.