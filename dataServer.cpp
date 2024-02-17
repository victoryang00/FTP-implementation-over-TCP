#include <cstring>
#include <deque>
#include <dirent.h>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <pthread.h>
#include <queue>
#include <stdlib.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>

#ifdef __wasi__
#include <wasi_socket_ext.h>
#endif

// Custom print error function
#define perror2(s, e) fprintf(stderr, "%s: %s\n", s, strerror(e))

using namespace std;
int fd;
// Custom print error function
void perror_exit(string message) {
    perror(message.c_str());
    exit(EXIT_FAILURE);
}

// An item in the queue contains the name of the file and the socket to which it must be sent
typedef struct queue_item {
    char file_name[4096];
    FILE *sock_fp;
} queueItem;

///////////////////////////////////////////////////////////////////////////////////////////////////////
////////// GLOBAL VARIABLES TO BE SHARED AMONG ALL THREADS AND FUNCTIONS //////////////////////////////
///////////// AN APPROACH USING ARGUMENTS WAS MUCH MUCH MORE COMPLEX //////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////
///
int block_size = 0; // Block size in bytes for the files that the workers send                      ///
                    ///
pthread_mutex_t queue_lock; // mutex for accessing the queue            ///
int queue_size = 0; ///
queue<queueItem> files_queue; // The queue containing the files to be sent                        ///
                              ///
// A map to match sockets with the number of files that must be sent through each socket            ///
// We need it in order to know when to close the socket (i.e. when files for this socket reach 0)   ///
map<FILE *, int> files_per_socket; ///
                                   ///
///////////////////////////////////////////////////////////////////////////////////////////////////////
///////////////////////////////////////////////////////////////////////////////////////////////////////

// send a file fp to a client through sock_fp using write() (size bytes in every write() call)
int write_file_to_client(int fd, FILE *fp, size_t size) {
    size_t n;
    int sent = 0;
    char *buffer = new char[size];

    while (sent<1e7) {
        // n = fread(buffer, 1, size, fp); // Read up to size bytes
        // if (n == 0) {
        //     // Either EOF is reached, or an error occurred
        //     if (feof(fp)) {
        //         // End of file
        //         break;
        //     }
        //     if (ferror(fp)) {
        //         // An error occurred
        //         perror("Error reading the file");
        //         break;
        //     }
        // }
        
        int n_sent = send(fd, buffer, size, 0); // Send the read bytes to the client
        if (n_sent < 0) {
            perror("send failed");
            break;
        }
        sent += n_sent;
    }

    delete[] buffer;
    return sent;
}

// explore files in a directory recursively and add them to the queue
void exploreFilesRecursively(char *base_path, queue<queueItem> &files_queue, FILE *sock_fp) {
    int err;
    char path[4096];
    memset(&path[0], 0, sizeof(path));
    struct dirent *dp;
    DIR *dir_stream = opendir(base_path); // open the desired directory stream

    if (!dir_stream) // return if NULL (directory stream could not be opened)
        return;

    while ((dp = readdir(dir_stream)) != NULL) { // get next directory entry
        if (strcmp(dp->d_name, ".") != 0 &&
            strcmp(dp->d_name, "..") != 0) { // if it is not the current or the previous directory

            // create new path based on the base_path given by the user and the directory entry currently read
            strcpy(path, base_path);
            strcat(path, "/");
            strcat(path, dp->d_name);

            if (dp->d_type != DT_DIR) { // if it is a file and not a directory
                while (1) {
                    // try to lock the queue
                    if (err = pthread_mutex_lock(&queue_lock)) {
                        perror2("pthread_mutex_lock", err);
                        exit(1);
                    }
                    if (files_queue.size() < queue_size) { // and if there is empty space
                        queueItem queue_item1; // create a new entry - struct
                        memset(&(queue_item1.file_name)[0], 0, 4096); // initialize file name as empty
                        strcpy(queue_item1.file_name, path); // copy the path to the new entry
                        queue_item1.sock_fp = sock_fp; // copy the socket file pointer to the new entry
                        files_queue.push(queue_item1); // and add it to the queue
                        // also update the map containing the number of files for this socket file pointer
                        files_per_socket.insert(pair<FILE *, int>(sock_fp, files_per_socket[sock_fp] += 1));
                        // try to unlock the queue
                        if (err = pthread_mutex_unlock(&queue_lock)) {
                            perror2("pthread_mutex_unlock", err);
                            exit(1);
                        }
                        break; // also assures that unlock is not performed twice
                    }
                    // try to unlock the queue
                    if (err = pthread_mutex_unlock(&queue_lock)) {
                        perror2("pthread_mutex_unlock", err);
                        exit(1);
                    }
                }
            }

            exploreFilesRecursively(path, files_queue, sock_fp); // continue resursive exploring according to new path
        }
    }

    closedir(dir_stream);
}

// code for communication threads
void *communication_thread(void *arg) {

    // get client socket file pointer and try to open it for both reading and writing
    FILE *sock_fp;
    int csock = *(int *)arg;
    if ((sock_fp = fdopen(csock, "r+")) == NULL)
        perror_exit("fdopen");

    // read from the socket the name of the directory that we want to explore and send
    char dirname[4096];
    if (recv(csock, dirname, BUFSIZ, 0) == NULL)
        perror_exit("reading dirname");
    // dirname[strcspn(dirname, "\n")] = '\0'; // find first '\n' occurence and terminate the string there
    printf("Sending directory name: %s\n", dirname);
    exploreFilesRecursively("./", files_queue, sock_fp);

    // now that all the directory has been explored recursively, wait for the workers to finish (i.e. 0 files for the
    // socket) and close the connection. Also erase the socket file pointer from the map.
    while (1) {
        if (files_per_socket[sock_fp] == 0) {
            if (fclose(sock_fp) != 0) {
                printf("close file");
            }
            map<FILE *, int>::iterator it;
            it = files_per_socket.find(sock_fp);
            files_per_socket.erase(it);
            break;
        }
    }

    // let the thread release its resources when it terminates
    int err;
    if (err = pthread_detach(pthread_self())) {
        perror2("pthread_detach", err);
        exit(1);
    }

    exit(0);
}

// code for worker threads
void *worker_thread(void *arg) {
    int c, err;
    while (1) {

        // try to lock the queue
        if (err = pthread_mutex_lock(&queue_lock)) {
            perror2("pthread_mutex_lock", err);
            exit(1);
        }

        if (files_queue.size() > 0) { // avoid empty queue

            // get first item of the queue
            queueItem queue_item1 = files_queue.front();

            // open the file described the file name (path)
            FILE *fp;
            fp = fopen(queue_item1.file_name, "r");
            fprintf(stderr, "Sending file: %s\n", queue_item1.file_name);
            // count file characters (to include it in the protocol)
            int count = 1e7;
            // for (c = getc(fp); c != EOF; c = getc(fp))
            //     count = count + 1;

           // get file descriptor of the socket file pointer

            // We define a protocol in which we deliver each file beginning with a '~', accompanied by the file name
            // the file name ends with '~' which is then followed by the number of characters of the file. Finally
            // one more '~' is sent and then the actual file contents are transferred.
            send(fd, "~", 1,0);

            send(fd, queue_item1.file_name, 4096,0);

            send(fd, "~", 1,0);

            char str[256]; // assume that the number of characters in a file can be converted to a 256 characters string
            memset(&str[0], 0, sizeof(str));
            sprintf(str, "%d", count);
            send(fd, str, sizeof(str),0);

            send(fd, "~", 1,0);

            write_file_to_client(fd, fp, block_size);

            // close the file described the file name (path)
            if (fclose(fp) != 0) {
                printf("close file");
            }

            files_queue.pop(); // pop the queue item we just processed
            // Update the corresponding map by decrementing the files for this socket by one
            files_per_socket.insert(pair<FILE *, int>(queue_item1.sock_fp, files_per_socket[queue_item1.sock_fp] -= 1));
        }

        // try to unlock the queue
        if (err = pthread_mutex_unlock(&queue_lock)) {
            perror2("pthread_mutex_unlock", err);
            exit(1);
        }
    }

    // let the thread release its resources when it terminates
    if (err = pthread_detach(pthread_self())) {
        perror2("pthread_detach", err);
        exit(1);
    }

    exit(0);
}

int main(int argc, char *argv[]) {

    int port = 0;
    int thread_pool_size = 0;

    // get command line arguments
    if (argc == 9) {
        // iterate through arguments - when position is an odd number we have a option and the next
        // argument is the variable for the option
        for (int i = 0; i < argc; i++) {
            if (i % 2) {
                if (!strcmp(argv[i], "-p")) {
                    port = atoi(argv[i + 1]);
                } else if (!strcmp(argv[i], "-s")) {
                    thread_pool_size = atoi(argv[i + 1]);
                } else if (!strcmp(argv[i], "-q")) {
                    queue_size = atoi(argv[i + 1]);
                } else if (!strcmp(argv[i], "-b")) {
                    block_size = atoi(argv[i + 1]);
                }
            }
        }
    } else {
        perror("wrong arguments");
    }

    // keep worker thread ids in a vector for further use
    vector<pthread_t> wtids;
    vector<pthread_t> ctids;

    pthread_mutex_init(&queue_lock, nullptr);
    // create thread_pool_size number of worker threads and add them to the vector
    int err;
    for (int i = 0; i < thread_pool_size; i++) {
        pthread_t returned;
        if (err = pthread_create(&returned, NULL, worker_thread, NULL)) {
            perror2("pthread_create", err);
            exit(1);
        }
        wtids.push_back(returned);
    }

    int lsock, csock;
    struct sockaddr_in myaddr;
    myaddr.sin_family = AF_INET;
    myaddr.sin_port = htons(port);
    myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
        auto addrlen =sizeof(myaddr);

    printf("[Server] Create socket\n");
    lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) {
        perror("Create socket failed");
    }

    printf("[Server] Bind socket\n");
    if (::bind(lsock, (struct sockaddr *)&myaddr, addrlen) < 0) {
        perror("Bind failed");
    }

    printf("[Server] Listening on socket\n");
    if (listen(lsock, 3) < 0) {
        perror("Listen failed");
    }

    while (true) {
        // extract the first connection request on the queue of pending connections for the listening socket,
        // create a new connected socket, and return a new file descriptor referring to that socket
        if ((csock = ::accept(lsock, (struct sockaddr *)&myaddr, (socklen_t *)&addrlen)) < 0)
            perror_exit("accept");
        fd = csock;
        // create new communication thread
        pthread_t returned;
        if (err = pthread_create(&returned, NULL, communication_thread, (void *)&csock)) {
            perror2("pthread_create", err);
            exit(1);
        }
        ctids.push_back(returned); // add it to the corresponding vector
    }

    return 0;
}