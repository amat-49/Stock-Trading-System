#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>

#define SERVER_PORT 6506
#define MAX_LINE 256

int s;
int client_running = 1;

// Function to display menu
void display_menu() {
    printf("\n========== Stock Trading System ==========\n");
    printf("Available Commands:\n");

    printf("LOGIN <userId> <password> \n");
    printf("    Example: LOGIN John John01\n\n");


    printf("BUY <stock_symbol> <amount> <price_per_stock>\n");
    printf("    Example: BUY MSFT 3.4 1.35\n\n");

    printf("SELL <stock_symbol> <amount> <price_per_stock>\n");
    printf("   Example: SELL APPL 2 1.45\n\n");


    printf("LIST\n");
    printf("BALANCE\n");

    printf("DEPOSIT <amount>\n");

    printf("LOOKUP <ticker>\n");
    printf("    Example: LOOKUP MSFT \n");

    printf("WHO (command only allowed for root user)\n");
    printf("LOGOUT\n");
    printf("QUIT\n");
    printf("SHUTDOWN (command only allowed for root user)\n");


    printf("==========================================\n");
    printf("Enter command: ");
    fflush(stdout);
}
void *receive_thread(void *arg)
{
    char response[MAX_LINE * 8];
    int len;

    while (client_running)
    {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(s, &read_set);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ready = select(s + 1, &read_set, NULL, NULL, &tv);

        if (ready < 0)
        {
            if (!client_running)
            {
                break;
            }
            perror("select (receive_thread)");
            break;
        }

        if (ready == 0)
        {
            continue;
        }

        if (FD_ISSET(s, &read_set))
        {
            memset(response, 0, sizeof(response));
            len = recv(s, response, sizeof(response) - 1, 0);

            if (len <= 0)
            {
                if (client_running)
                {
                    printf("\nServer closed the connection.\n");
                }
                client_running = 0;
                break;
            }

            response[len] = '\0';
            printf("\n%s\n", response);
            fflush(stdout);

            if (strstr(response, "Server shutting down") != NULL)
            {
                client_running = 0;
                break;
            }

            if (client_running)
            {
                display_menu();
            }
        }
    }
    return NULL;
}

int main(int argc, char* argv[]) {
    struct hostent* hp;
    struct sockaddr_in sin;
    char* host;
    char buf[MAX_LINE];
    pthread_t recv_tid;

    if (argc == 2) {
        host = argv[1];
    } else {
        fprintf(stderr, "Usage: %s <hostname>\n", argv[0]);
        fprintf(stderr, "Example: %s localhost\n", argv[0]);
        exit(1);
    }

    /* translate host name into peer's IP address */
    hp = gethostbyname(host);
    if (!hp) {
        fprintf(stderr, "client: unknown host: %s\n", host);
        exit(1);
    }

    /* build address data structure */
    bzero((char*)&sin, sizeof(sin));
    sin.sin_family = AF_INET;
    bcopy(hp->h_addr, (char*)&sin.sin_addr, hp->h_length);
    sin.sin_port = htons(SERVER_PORT);

    /* active open */
    if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("client: socket");
        exit(1);
    }
    
    printf("Connecting to server at %s:%d...\n", host, SERVER_PORT);
    
    if (connect(s, (struct sockaddr*)&sin, sizeof(sin)) < 0) {
        perror("client: connect");
        close(s);
        exit(1);
    }
    
    printf("Connected to server successfully!\n");


    /* start the background thread that receives and prints server messages*/
    if(pthread_create(&recv_tid, NULL, receive_thread, NULL) != 0)
    {
        perror("pthread_create");
        close(s);
        exit(1);
    }
    display_menu();

    /* main loop: display menu, get commands, send to server, and receive responses */
    while (client_running) {

        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);

        /* 1 sec timeout so can recheck client running if server shuts donw*/
        struct timeval tv;
        tv.tv_sec = 1; 
        tv.tv_usec = 0;

        int ready = select(STDIN_FILENO + 1, &read_set, NULL, NULL, &tv);

        if(ready < 0)
        {
            perror("select (main)");
            break;
        }

        if(ready == 0)
        {
            /* timeout loop back to check client running*/
            continue;
        }

        if(FD_ISSET(STDIN_FILENO, &read_set))
        {
            if(fgets(buf, sizeof(buf), stdin) == NULL)
            {
                /*EOF on stdin*/
                break;
            }

            //if blank lines then skip them
            if(strlen(buf) == 0 || strcmp(buf, "\n") == 0)
            {
                continue;
            }

            /*send the command to the server*/
            if(send(s, buf, strlen(buf), 0) < 0)
            {
                perror("client: send");
                break;
            }

            //QUIT exists the client immediately after sending
            if(strncmp(buf, "QUIT", 4) == 0)
            {
                client_running = 0;
                break;
            }
            
        }
    }

    client_running = 0;
    shutdown(s, SHUT_RDWR);
    close(s);

    pthread_join(recv_tid, NULL);
    printf("Client terminated.\n");
    return 0;
}