#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <stdbool.h>
#include <pthread.h>   
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
    
/* A big size for mallocing space */
#define MAXIMUM_SIZE 100

/* Enum for returning errors */
typedef enum {
    OK = 0,
    USAGE = 1,
    INVCONTENTS = 2,
    INVQUANTITY = 3
} Err2;

/* A function that takes an error and converts it into a string for
   returning  
*/
Err2 showerr(Err2 e) {
    const char* error = "";
    switch(e) {
        case OK:
            break;
        case USAGE:
            error = "Usage: 2310depot name {goods qty}\n";
            break;  
        case INVCONTENTS:
            error = "Invalid name(s)\n";
            break; 
        case INVQUANTITY:
            error = "Invalid quantity\n";
            break;    
    }
    fprintf(stderr, "%s", error);
    return e;
}

/* Struct to pair a material with a quantity */
typedef struct {
    char* materialName;
    int quantity;
} Material;

/* Struct to centralise data for incoming connections */
typedef struct {
    int connectionPort;
    char* connectionName;
    FILE* read;
    FILE* write;
} Connection;

/* Struct for all of the game data */
typedef struct {
    char* depotname;
    Material* materials;
    int numberOfMaterials;
    Connection** connections;
    int numberOfConnections;
    pthread_t tid[MAXIMUM_SIZE];
    int mostRecent;
    int myPort;
    pthread_mutex_t lock;
    char*** queue;
    int numberOfQueues;
} GameData;

/* A function that takes a void pointer and returns a void pointer,
sleeps on a loop waiting for a signal to come in
 */
void* counter(void* v) {
    for(int i = 0; i < 10000; ++i) {
        sleep(2);
    }
    return 0;
}

/* A function that takes the game data and prints the materials of this depot 
in lexicographic order
 */
void print_sort(GameData* game) {
    char** gathering = malloc(sizeof(char*) * game->numberOfMaterials);
    for(int i = 0; i < game->numberOfMaterials; ++i) {
        gathering[i] = malloc(sizeof(char) * MAXIMUM_SIZE);
    }
    for(int i = 0; i < game->numberOfMaterials; ++i) {
        strcpy(gathering[i], game->materials[i].materialName);
    }
    char* temp = malloc(sizeof(char) * MAXIMUM_SIZE);
    for (int i = 0; i < game->numberOfMaterials; i++) {
        for (int i2 = i + 1; i2 < game->numberOfMaterials; i2++) {
            if(strcmp(gathering[i], gathering[i2]) > 0) {
                strcpy(temp, gathering[i]);
                strcpy(gathering[i], gathering[i2]);
                strcpy(gathering[i2], temp);
            }
        }
    }
    for(int i = 0; i < game->numberOfMaterials; ++i) {
        printf("%s ", gathering[i]);
        fflush(stdout);
        for(int j = 0; j < game->numberOfMaterials; ++j) {
            if(strcmp(gathering[i], game->materials[j].materialName) == 0) {
                printf("%d\n", game->materials[j].quantity);
                fflush(stdout);
            }
        }
    }
}

/* A function that takes the name of a material and game data and returns 1 if
this depot has it, 0 if not */
int do_i_have_good(char* name, GameData* game) {
    for(int i = 0; i < game->numberOfMaterials; ++i) {
        if(strcmp(game->materials[i].materialName, name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* if a good is 0, remove it from the depot */
void if_good_is_0(GameData* game) {
    for(int i = 0; i < game->numberOfMaterials; ++i) {
        if(game->materials[i].quantity == 0) {
            game->materials[i] = game->materials[i + 1];
            game->numberOfMaterials--;
        }
    }
}

/* Withdraw an amount of a material from this depot, if the depot does not have
the good add it and put it into negative
 */
void withdraw(GameData* game, char* materialName, int amount) {
    if(do_i_have_good(materialName, game)) {
        for(int i = 0; i < game->numberOfMaterials; ++i) {
            if(strcmp(game->materials[i].materialName, materialName) == 0) {
                game->materials[i].quantity -= amount;
                if_good_is_0(game);
            }
        }
    } else {
        strcpy(game->materials[game->numberOfMaterials].materialName,
                materialName);
        game->materials[game->numberOfMaterials].quantity = 0 - amount;
        game->numberOfMaterials++;
    }
}

/* A function to validate a withdraw message from the network before sending it
to the withdraw function
*/
int handle_withdraw(GameData* game, char* name) {
    int delimCounter = 0;
    for(int i = 0; i < (int)strlen(name); ++i) {
        if(name[i] == ':') {
            delimCounter++;
        }
    }
    if(delimCounter != 2) {
        return 1;
    }
    char* delim = ":";
    int amount = 0;
    int incorrectQuantity = 0;
    char* token = malloc(sizeof(char) * MAXIMUM_SIZE);
    token = strtok(name, delim);
    token = strtok(NULL, delim);
    for(int i = 0; i < (int)strlen(token); ++i) {
        if(!isdigit(token[i])) {
            incorrectQuantity = 1;
        }
    }
    amount = atoi(token);
    token = strtok(NULL, delim);
    if(!incorrectQuantity && amount > 0) {
        withdraw(game, token, amount);
    }
    return 0;
}

/* Deliver a good to this depot, if the depot has the good already then add to
it otherwise add the good to this depot 
*/
void handle_deliver(GameData* game, char* name, int amount) {
    int index = 0;
    if(do_i_have_good(name, game)) {
        for(int i = 0; i < game->numberOfMaterials; ++i) {
            if(strcmp(game->materials[i].materialName, name) == 0) {
                index = i;
            }
        }
        game->materials[index].quantity += amount;
    } else {
        strcpy(game->materials[game->numberOfMaterials].materialName, name);
        game->materials[game->numberOfMaterials].quantity = amount;
        game->numberOfMaterials++;
    }
}

/* Validate a message to deliver to this depot before processing it */
int deliver_msg(char* buffer, GameData* game) {
    int delimCounter = 0;
    for(int i = 0; i < (int)strlen(buffer); ++i) {
        if(buffer[i] == ':') {
            delimCounter++;
        }
    }
    if(delimCounter != 2) {
        return 1;
    }
    char* delim = ":";
    int incorrectQuantity = 0;
    int quantity = 0;
    char* name = malloc(sizeof(char) * MAXIMUM_SIZE);
    char* token = malloc(sizeof(char) * MAXIMUM_SIZE);
    if(strncmp(buffer, "Deliver", 7) == 0) {
        token = strtok(buffer, delim);
        token = strtok(NULL, delim);
        for(int i = 0; i < (int)strlen(token); ++i) {
            if(!isdigit(token[i])) {
                incorrectQuantity = 1;
            }
        }
        quantity = atoi(token);
        token = strtok(NULL, delim);
        name = token;
        token = strtok(NULL, delim);
        if(token == NULL) {
            if(incorrectQuantity == 0) {
                handle_deliver(game, name, quantity);
            }
        }
        return 1;
    } else {
        return 0;
    }
}

/* Check that a string does not contain a \n, \r or a : in it */
int check_name(char* name) {
    for(int i = 0; i < strlen(name); ++i) {
        if(name[i] == '\n' || name[i] == '\r' || name[i] == ':' || 
                name[i] == ' ') {
            return 1;
        }
    }
    return 0;
}

/* Print all of this depots neighbours in lexicographic order */
void lexicographic_neighbours(GameData* game) {
    char** gatherNeighbours = malloc(sizeof(char*) * MAXIMUM_SIZE);
    for(int i = 0; i < MAXIMUM_SIZE; ++i) {
        gatherNeighbours[i] = malloc(sizeof(char) * MAXIMUM_SIZE);
    }
    for(int i = 0; i < game->numberOfConnections; ++i) {
        strcpy(gatherNeighbours[i], game->connections[i]->connectionName);
    }
    char* temp = malloc(sizeof(char) * MAXIMUM_SIZE);
    for (int i = 0; i < game->numberOfConnections; i++) {
        for (int i2 = i + 1; i2 < game->numberOfConnections; i2++) {
            if(strcmp(gatherNeighbours[i], gatherNeighbours[i2]) > 0) {
                strcpy(temp, gatherNeighbours[i]);
                strcpy(gatherNeighbours[i], gatherNeighbours[i2]);
                strcpy(gatherNeighbours[i2], temp);
            }
        }
    }
    for(int i = 0; i < game->numberOfConnections; ++i) {
        if((int)strlen(gatherNeighbours[i]) == 0) {
        } else {
            printf("%s\n", gatherNeighbours[i]);
            fflush(stdout);
        }
    }
}

/* Setup a signal handler that prints the goods and neighbours in lexicographic
when the program recieves SIGHUP
 */
void* sigmund(void* v) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGPIPE);
    int num;
    while (!sigwait(&set, &num)) {
        if(num == 1) {
            GameData* temp = (GameData*)v;
            printf("Goods:\n");
            fflush(stdout);
            print_sort(temp);
            fflush(stdout);
            printf("Neighbours:\n");
            fflush(stdout);
            lexicographic_neighbours(temp);
            fflush(stdout);
        }
    }
    return 0;
}

/* Validate an IM message from a connection */
int is_it_im(char* buffer, Connection* conn, GameData* game) {
    int tokenCounter = 0;
    char* token = malloc(sizeof(char) * MAXIMUM_SIZE);
    char* delim = ":";
    token = strtok(buffer, delim);
    if(strcmp(token, "IM") == 0) {
        tokenCounter++;
    }
    token = strtok(NULL, delim);
    for(int i = 0; i < (int)strlen(token); ++i) {
        if(!isdigit(token[i])) {
            pthread_mutex_unlock(&(game->lock));
            pthread_exit(0);
        }
    }
    conn->connectionPort = atoi(token);
    token = strtok(NULL, delim);
    if(check_name(token)) {
        pthread_mutex_unlock(&(game->lock));
        pthread_exit(0);
    } else {
        strcpy(conn->connectionName, token);
    }
    return 0;
}

int connect_to(int port, GameData* game);

/* Validate a connection message before processing it */
int connection_msg(char* msg, GameData* game) {
    pthread_mutex_lock(&(game->lock));
    char* token = malloc(sizeof(char) * MAXIMUM_SIZE);
    token = strtok(msg, ":");
    token = strtok(NULL, ":");
    if(token != NULL) {
        for(int i = 0; i < (int)strlen(token); ++i) {
            if(!isdigit(token[i])) {
                pthread_mutex_unlock(&(game->lock));
                return 1;
            }
        }
        for(int i = 0; i < game->numberOfConnections; ++i) {
            if(atoi(token) == game->connections[i]->connectionPort) {
                pthread_mutex_unlock(&(game->lock));
                return 1;
            }
        }
        connect_to(atoi(token), game);
    }
    return 0;
}

/* Check if this depot is already connected to another depot of name name */
int am_i_connected_to(GameData* game, char* name) {
    int connected = -1;
    for(int i = 0; i < game->numberOfConnections; ++i) {
        if(strcmp(game->connections[i]->connectionName, name) == 0) {
            connected = i;
        }
    }
    return connected;
}

/* Withdraw a quantity from this depot and deliver it to the write stream of
another connection
*/
int transfer(GameData* game, char* material, int quantity, char* destination) {
    withdraw(game, material, quantity);
    int checker = am_i_connected_to(game, destination);
    if(checker > -1) {
        fprintf(game->connections[checker]->write, "Deliver:%d:%s\n",
                quantity, material);
        fflush(game->connections[checker]->write);
    }
    return 0;
}

/* Valide a message to transfer before processing the operation */
int transfer_msg(GameData* game, char* msg) {
    int delimCounter = 0;
    for(int i = 0; i < (int)strlen(msg); ++i) {
        if(msg[i] == ':') {
            delimCounter++;
        }
    }
    if(delimCounter != 3) {
        return 1;
    }
    char* message = malloc(sizeof(char) * MAXIMUM_SIZE);
    char* matName = malloc(sizeof(char) * MAXIMUM_SIZE);
    char* dest = malloc(sizeof(char) * MAXIMUM_SIZE);
    int quantity = 0;
    message = strtok(msg, ":");
    message = strtok(NULL, ":");
    for(int i = 0; i < (int)strlen(message); ++i) {
        if(!isdigit(message[i])) {
            return 1;
        }
    }
    quantity = atoi(message);
    message = strtok(NULL, ":");
    if(check_name(message)) {
        return 1;
    }
    strcpy(matName, message);
    message = strtok(NULL, ":");
    if(check_name(message)) {
        return 1;
    }
    strcpy(dest, message);
    transfer(game, matName, quantity, dest);
    return 0;
}

/* Check if a defer key is already has a queue */
int check_queue(GameData* game, int queueNumber) {
    int keyindex = -1;
    for(int i = 0; i < game->numberOfQueues; ++i) {
        if(queueNumber == atoi(game->queue[i][0])) {
            keyindex = i;
        }
    }
    return keyindex;
}

/* Add a key to the defer message queue, if it exists then add message to queue
, if it does not then add it as a new key
*/
void add_key_to_queue(GameData* game, int keynum, char* msg) {
    int a = check_queue(game, keynum);
    char* stringkeynum = malloc(sizeof(char) * MAXIMUM_SIZE);
    sprintf(stringkeynum, "%d", keynum);
    if(a > -1) {
        int queuelength = 0;
        char* tester = malloc(sizeof(char) * MAXIMUM_SIZE);
        strcpy(tester, game->queue[a][0]);
        while(strcmp(tester, "\n") != 0) {
            strcpy(tester, game->queue[a][queuelength]);
            queuelength++;
        }
        strcpy(game->queue[a][queuelength - 1], msg);
        strcpy(game->queue[a][queuelength], "\n");
    } else {
        strcpy(game->queue[game->numberOfQueues][0], stringkeynum);
        strcpy(game->queue[game->numberOfQueues][1], msg);
        strcpy(game->queue[game->numberOfQueues][2], "\n");
        game->numberOfQueues++;
    }
}

/* Validate a defer message before processing it */
int defer_msg(GameData* game, char* msg) {
    char* token = malloc(sizeof(char) * MAXIMUM_SIZE);
    char* copy = malloc(sizeof(char) * MAXIMUM_SIZE);
    strcpy(copy, msg);
    int key;
    token = strtok(msg, ":");
    token = strtok(NULL, ":");
    for(int i = 0; i < (int)strlen(token); ++i) {
        if(!isdigit(token[i])) {
            return 1;
        }
    }
    key = atoi(token);
    if(key < 0) {
        return 1;
    }
    add_key_to_queue(game, key, copy);
    return 0;
}

/* Remove a defer key and its operations */
void remove_queue_key(GameData* game, int index) {
    for(int i = index; i < game->numberOfQueues; ++i) {
        game->queue[i] = game->queue[i + 1];
        game->numberOfQueues--;
    }
}

/* Execute a queue of commands on a defer key */
int execute(GameData* game, char* key, char* msg) {
    char* keycopy = malloc(sizeof(char) * MAXIMUM_SIZE);
    strcpy(keycopy, key);
    int a = check_queue(game, atoi(key));
    int amounterOfInstructions = 1;
    char* temp = malloc(sizeof(char) * MAXIMUM_SIZE);
    temp = game->queue[a][0];
    int transferFlag = 0;
    int failFlag = 0;
    while(strcmp(temp, "\n") != 0) {
        strcpy(temp, game->queue[a][amounterOfInstructions]);
        if(!strcmp(temp, "\n")) {
            break;
        }
        amounterOfInstructions++;
        char* tempInstruction = malloc(sizeof(char) * MAXIMUM_SIZE);
        tempInstruction = strtok(temp, ":");
        tempInstruction = strtok(NULL, ":");
        tempInstruction = strtok(NULL, ":");
        if(!strcmp(tempInstruction, "Transfer")) {
            transferFlag = 1;
        }
        if(tempInstruction != NULL) {
            char* finalInstruction = malloc(sizeof(char) * MAXIMUM_SIZE);
            finalInstruction[0] = '\0';
            strcat(finalInstruction, tempInstruction);
            strcat(finalInstruction, ":");
            tempInstruction = strtok(NULL, ":");
            if(tempInstruction != NULL) {
                strcat(finalInstruction, tempInstruction);
                strcat(finalInstruction, ":");
                tempInstruction = strtok(NULL, ":");
                if(tempInstruction != NULL) {
                    strcat(finalInstruction, tempInstruction);
                    if(transferFlag) {
                        tempInstruction = strtok(NULL, ":");
                        if(tempInstruction != NULL) {
                            strcat(finalInstruction, ":");
                            strcat(finalInstruction, tempInstruction);
                            tempInstruction = strtok(NULL, ":");
                            if(tempInstruction != NULL) {
                                failFlag = 1;
                            }
                        }
                    }
                    if(!strncmp(finalInstruction, "Deliver", 7)) {
                        if((int)strlen(finalInstruction) > 11) {
                            deliver_msg(finalInstruction, game);
                        }
                    }
                    if(!strncmp(finalInstruction, "Withdraw", 8)) {
                        if((int)strlen(finalInstruction) > 12) {
                            handle_withdraw(game, finalInstruction);
                        }
                    }
                    if(!strncmp(finalInstruction, "Transfer", 7)) {
                        if(!failFlag) {
                            transfer_msg(game, finalInstruction);
                        }

                    }
                }
            }
        }
    }
    transferFlag = 0;
    remove_queue_key(game, a);
    return 1;
}

/* Validate an execute message before processing it */
int execute_msg(GameData* game, char* msg) {
    int len = strlen(msg);
    if(len < 9) {
        return 1;
    }
    char* token = strtok(msg, ":");
    char* key = malloc(sizeof(char) * MAXIMUM_SIZE);
    if(!strncmp(msg, "Execute:", 8)) {
        return 1;
    }
    token = strtok(NULL, ":");
    for(int i = 0; i < (int)strlen(token); ++i) {
        if(!isdigit(token[i])) {
            return 1;
        }
    }
    int a = check_queue(game, atoi(token));
    if(a == -1) {
        return 1;
    }
    strcpy(key, token);
    execute(game, key, msg);
    return 0;
}

/* Create an instance of a Connection struct and add it to the array of 
connections once the connection confirms with an IM message, once connected 
wait for a message to be sent and handle it
*/
void* handle_connection(void* voidGameData) {
    int introduction = 0;
    int failflag = 0;
    GameData* game = (GameData*)voidGameData;
    Connection* conn = malloc(sizeof(Connection));
    conn->connectionName = malloc(sizeof(char) * MAXIMUM_SIZE);
    conn->read = fdopen(dup(game->mostRecent), "r");
    conn->write = fdopen(dup(game->mostRecent), "w");
    conn->connectionPort = 0;
    fprintf(conn->write, "IM:%u:%s\n", game->myPort, game->depotname);
    fflush(conn->write);
    char buffer[MAXIMUM_SIZE];
    fflush(conn->read);
    while(fgets(buffer, MAXIMUM_SIZE, conn->read)) {
        if((strcmp(buffer, "\n") > 0) && (strcmp(buffer, "\0") > 0)) {
            if(buffer[strlen(buffer) - 1] == '\n') {
                buffer[strlen(buffer) - 1] = '\0';
                if(introduction == 0) {
                    if(!strncmp(buffer, "IM:", 3)) {
                        if(!is_it_im(buffer, conn, game)) {
                            game->connections[game->numberOfConnections] 
                                    = conn;
                            game->numberOfConnections++;
                            introduction = 1;
                        }
                        pthread_mutex_unlock(&(game->lock));
                    } else {
                        introduction = 1;
                        failflag = 1;
                        pthread_mutex_unlock(&(game->lock));
                    }
                } else {
                    if(!failflag) { 
                        if(!strncmp(buffer, "Deliver", 7)) {
                            pthread_mutex_lock(&(game->lock));
                            deliver_msg(buffer, game);
                            pthread_mutex_unlock(&(game->lock));
                        }
                        if(!strncmp(buffer, "Withdraw", 8)) {
                            pthread_mutex_lock(&(game->lock));
                            handle_withdraw(game, buffer);
                            pthread_mutex_unlock(&(game->lock));
                        }
                        if(!strncmp(buffer, "Connect", 7)) {
                            connection_msg(buffer, game);
                        }
                        if(!strncmp(buffer, "Transfer", 8)) {
                            pthread_mutex_lock(&(game->lock));
                            transfer_msg(game, buffer);
                            pthread_mutex_unlock(&(game->lock));
                        }
                        if(!strncmp(buffer, "Defer", 5)) {
                            pthread_mutex_lock(&(game->lock));
                            defer_msg(game, buffer);
                            pthread_mutex_unlock(&(game->lock));
                        }
                        if(!strncmp(buffer, "Execute", 7)) {
                            pthread_mutex_lock(&(game->lock));
                            execute_msg(game, buffer);
                            pthread_mutex_unlock(&(game->lock));
                        }
                    }
                }
            }   
        }
    }
    return 0;
}

/* Attempt to connect to a port */
int connect_to(int port, GameData* game) {
    int fd;
    struct sockaddr_in socketAddr;
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0) {
        return 1;
    }
    socketAddr.sin_family = AF_INET;
    socketAddr.sin_port = htons(port);
    socketAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    int a = connect(fd, (struct sockaddr*)&socketAddr, sizeof(socketAddr));
    if(a < 0) {
        fflush(stdout);
        return 1;
    }
    FILE* write = fdopen(dup(fd), "w");
    fflush(write);
    fclose(write);
    game->mostRecent = fd;
    pthread_create(&game->tid[game->numberOfConnections], 0, handle_connection,
            game);
    return 0;
}

/* Listen on a port and wait for a connection to come in */
int network_code(GameData* game) {
    struct addrinfo* ai = 0;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;   
    int err;
    if ((err = getaddrinfo("localhost", 0, &hints, &ai))) {
        freeaddrinfo(ai);
        fprintf(stderr, "%s\n", gai_strerror(err));
        return 1;
    }
    int serv = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(serv, (struct sockaddr*)ai->ai_addr, sizeof(struct sockaddr))) {
        perror("Binding");
        return 3;
    }
    struct sockaddr_in ad;
    memset(&ad, 0, sizeof(struct sockaddr_in));
    socklen_t len = sizeof(struct sockaddr_in);
    if (getsockname(serv, (struct sockaddr*)&ad, &len)) {
        perror("sockname");
        return 4;
    }
    printf("%u\n", ntohs(ad.sin_port));
    fflush(stdout);
    game->myPort = ntohs(ad.sin_port);
    if (listen(serv, 10)) {
        perror("Listen");
        return 4;
    }
    int connFD;
    while (connFD = accept(serv, 0, 0), connFD >= 0) {
        pthread_mutex_lock(&(game->lock));
        game->mostRecent = connFD;
        pthread_create(&game->tid[game->numberOfConnections], 0,
                handle_connection, game);
    }

    return 0;
}

/* Validate the argv arguments before starting the program */
int check_argv(char** contents, int argc, GameData* game) {

    for(int i = 2; i < argc; ++i) {

        if(strlen(contents[i]) == 0) {
            if(i % 2 == 0) {
                return 1;
            } else {
                return 2;
            }
        }
        if(i % 2 == 0) {
            if(check_name(contents[i])) {
                return 1;
            } else {
                game->materials[game->numberOfMaterials].materialName
                        = contents[i];
            }
        }
        if(i % 2 == 1) {
            for(int i2 = 0; i2 < (int)strlen(contents[i]); ++i2) {
                if(!isdigit(contents[i][i2])) {
                    return 2;
                    if(atoi(contents[i]) < 1) {
                        return 2;
                    }
                }
            }    
            game->materials[game->numberOfMaterials].quantity
                    = atoi(contents[i]);
            game->numberOfMaterials++;
        }
    }
    return 0;
}

/* Setup the space for the starting structs, signal handler and begin listening
for connections on a port
*/
int main(int argc, char** argv) {
    if(argc < 2 || (argc % 2 == 1)) {
        return showerr(USAGE);
    }
    GameData* game = malloc(sizeof(GameData));
    game->numberOfMaterials = 0;
    game->numberOfConnections = 0;
    game->materials = malloc(sizeof(Material) * MAXIMUM_SIZE);
    game->queue = malloc(sizeof(char**) * MAXIMUM_SIZE);
    for(int i = 0; i < MAXIMUM_SIZE; i++) {
        game->queue[i] = malloc(sizeof(char*) * MAXIMUM_SIZE);
        for(int j = 0; j < MAXIMUM_SIZE; j++) {
            game->queue[i][j] = malloc(sizeof(char) * 20);
        }
    }
    game->numberOfQueues = 0;
    pthread_mutex_init((&game->lock), NULL);
    for(int i = 0; i < MAXIMUM_SIZE; ++i) {
        game->materials[i].materialName = malloc(sizeof(char) * MAXIMUM_SIZE);
    }
    game->connections = malloc(sizeof(Connection) * MAXIMUM_SIZE);
    if(check_name(argv[1])) {
        return showerr(INVCONTENTS);
    } else {
        game->depotname = argv[1];
    }
    int argcCheck = check_argv(argv, argc, game);
    if(argcCheck == 1) {
        return showerr(INVCONTENTS);
    } else if (argcCheck == 2) {
        return showerr(INVQUANTITY);
    }
    pthread_t tid[3];
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, 0);    
    pthread_create(&tid[0], 0, sigmund, (void*)game); 
    pthread_create(&tid[1], 0, counter, "1\n");
    pthread_create(&tid[2], 0, counter, "2\n");
    network_code(game);
    pthread_mutex_destroy(&(game->lock));
}
