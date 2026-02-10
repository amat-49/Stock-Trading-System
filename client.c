#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>

#define SERVER_PORT 5432
#define MAX_LINE 256

// Function to display menu
void display_menu() {
    printf("\n========== Stock Trading System ==========\n");
    printf("Available Commands:\n");
    printf("1. BUY <stock_symbol> <amount> <price_per_stock> <user_id>\n");
    printf("   Example: BUY MSFT 3.4 1.35 1\n\n");
    printf("2. SELL <stock_symbol> <amount> <price_per_stock> <user_id>\n");
    printf("   Example: SELL APPL 2 1.45 1\n\n");
    printf("3. LIST [user_id]\n");
    printf("   Example: LIST 1 (or just LIST for default user)\n\n");
    printf("4. BALANCE [user_id]\n");
    printf("   Example: BALANCE 1 (or just BALANCE for default user)\n\n");
    printf("5. QUIT - Close client connection\n");
    printf("6. SHUTDOWN - Shutdown the server\n");
    printf("==========================================\n");
    printf("Enter command: ");
}

int main(int argc, char* argv[]) {
    struct hostent* hp;
    struct sockaddr_in sin;
    char* host;
    char buf[MAX_LINE];
    char response[MAX_LINE * 4];
    int s;
    int len;

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

    /* main loop: display menu, get commands, send to server, and receive responses */
    while (1) {
        display_menu();
        fflush(stdout);  // Force output immediately
        
        // Get input from user
        if (fgets(buf, sizeof(buf), stdin) == NULL) {
            break;
        }
        
        // Remove newline if present
        size_t buf_len = strlen(buf);
        if (buf_len > 0 && buf[buf_len - 1] == '\n') {
            buf[buf_len - 1] = '\0';
        }
        
        // Skip empty input
        if (strlen(buf) == 0) {
            continue;
        }
        
        // Add newline back for server processing
        strcat(buf, "\n");
        
        // Send command to server immediately
        len = strlen(buf);
        if (send(s, buf, len, 0) < 0) {
            perror("client: send");
            break;
        }
        fflush(NULL);  // Flush all output buffers
        
        // Receive response from server
        memset(response, 0, sizeof(response));
        len = recv(s, response, sizeof(response) - 1, 0);
        if (len <= 0) {
            printf("Server closed connection\n");
            break;
        }
        
        response[len] = '\0';
        printf("\n%s\n", response);
        fflush(stdout);
        
        // Check if it was a QUIT or SHUTDOWN command
        if (strncmp(buf, "QUIT", 4) == 0 || strncmp(buf, "SHUTDOWN", 8) == 0) {
            printf("Closing connection...\n");
            break;
        }
    }

    close(s);
    printf("Client terminated.\n");
    return 0;
}

