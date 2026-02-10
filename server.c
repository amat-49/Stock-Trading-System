#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include "sqlite3.h"

#define SERVER_PORT  5432
#define MAX_PENDING  5
#define MAX_LINE     256

// Global database pointer
sqlite3 *db;

// Function to initialize the database and create tables
int init_database() {
    char *err_msg = 0;
    int rc;

    // Open database (creates if doesn't exist)
    rc = sqlite3_open("stocks.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    // Create Users table
    const char *sql_users = 
        "CREATE TABLE IF NOT EXISTS Users ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
        "first_name TEXT, "
        "last_name TEXT, "
        "user_name TEXT NOT NULL, "
        "password TEXT, "
        "usd_balance DOUBLE NOT NULL);";

    rc = sqlite3_exec(db, sql_users, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 1;
    }

    // Create Stocks table
    const char *sql_stocks = 
        "CREATE TABLE IF NOT EXISTS Stocks ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
        "stock_symbol VARCHAR(4) NOT NULL, "
        "stock_name VARCHAR(20) NOT NULL, "
        "stock_balance DOUBLE, "
        "user_id INTEGER, "
        "FOREIGN KEY (user_id) REFERENCES Users (ID));";

    rc = sqlite3_exec(db, sql_stocks, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 1;
    }

    // Check if there are any users
    sqlite3_stmt *stmt;
    const char *check_users = "SELECT COUNT(*) FROM Users;";
    rc = sqlite3_prepare_v2(db, check_users, -1, &stmt, 0);
    
    if (rc == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int count = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            
            // If no users, create a default user
            if (count == 0) {
                const char *insert_user = 
                    "INSERT INTO Users (first_name, last_name, user_name, password, usd_balance) "
                    "VALUES ('Robby', 'Bobby', 'Rob_bob', 'password123', 100.0);";
                
                rc = sqlite3_exec(db, insert_user, 0, 0, &err_msg);
                if (rc != SQLITE_OK) {
                    fprintf(stderr, "SQL error: %s\n", err_msg);
                    sqlite3_free(err_msg);
                    return 1;
                }
                printf("Created default user: Robby Bobby with $100 balance\n");
            }
        }
    }

    return 0;
}

// Function to handle BUY command
void handle_buy(int socket, char *stock_symbol, double stock_amount, double price_per_stock, int user_id, char *response) {
    sqlite3_stmt *stmt;
    int rc;
    
    // Calculate total cost
    double total_cost = stock_amount * price_per_stock;
    
    // Check user's balance
    const char *check_balance = "SELECT usd_balance, first_name, last_name FROM Users WHERE ID = ?;";
    rc = sqlite3_prepare_v2(db, check_balance, -1, &stmt, 0);
    
    if (rc != SQLITE_OK) {
        sprintf(response, "403 message format error\nDatabase error\n");
        return;
    }
    
    sqlite3_bind_int(stmt, 1, user_id);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        double current_balance = sqlite3_column_double(stmt, 0);
        const char *first_name = (const char*)sqlite3_column_text(stmt, 1);
        const char *last_name = (const char*)sqlite3_column_text(stmt, 2);
        sqlite3_finalize(stmt);
        
        // Check if user has enough balance
        if (current_balance < total_cost) {
            sprintf(response, "403 message format error\nNot enough USD balance. Current balance: $%.2f, Required: $%.2f\n", 
                    current_balance, total_cost);
            return;
        }
        
        // Deduct from user's balance
        const char *update_balance = "UPDATE Users SET usd_balance = usd_balance - ? WHERE ID = ?;";
        rc = sqlite3_prepare_v2(db, update_balance, -1, &stmt, 0);
        sqlite3_bind_double(stmt, 1, total_cost);
        sqlite3_bind_int(stmt, 2, user_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        // Check if stock record exists for this user
        const char *check_stock = "SELECT stock_balance FROM Stocks WHERE stock_symbol = ? AND user_id = ?;";
        rc = sqlite3_prepare_v2(db, check_stock, -1, &stmt, 0);
        sqlite3_bind_text(stmt, 1, stock_symbol, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, user_id);
        
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            // Stock exists, update it
            double current_stock = sqlite3_column_double(stmt, 0);
            sqlite3_finalize(stmt);
            
            const char *update_stock = "UPDATE Stocks SET stock_balance = stock_balance + ? WHERE stock_symbol = ? AND user_id = ?;";
            rc = sqlite3_prepare_v2(db, update_stock, -1, &stmt, 0);
            sqlite3_bind_double(stmt, 1, stock_amount);
            sqlite3_bind_text(stmt, 2, stock_symbol, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 3, user_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            
            double new_stock_balance = current_stock + stock_amount;
            double new_usd_balance = current_balance - total_cost;
            sprintf(response, "200 OK\nBOUGHT: New balance: %.2f %s. USD balance $%.2f\n", 
                    new_stock_balance, stock_symbol, new_usd_balance);
        } else {
            // Stock doesn't exist, create new record
            sqlite3_finalize(stmt);
            
            const char *insert_stock = 
                "INSERT INTO Stocks (stock_symbol, stock_name, stock_balance, user_id) VALUES (?, ?, ?, ?);";
            rc = sqlite3_prepare_v2(db, insert_stock, -1, &stmt, 0);
            sqlite3_bind_text(stmt, 1, stock_symbol, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, stock_symbol, -1, SQLITE_STATIC);
            sqlite3_bind_double(stmt, 3, stock_amount);
            sqlite3_bind_int(stmt, 4, user_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            
            double new_usd_balance = current_balance - total_cost;
            sprintf(response, "200 OK\nBOUGHT: New balance: %.2f %s. USD balance $%.2f\n", 
                    stock_amount, stock_symbol, new_usd_balance);
        }
    } else {
        sqlite3_finalize(stmt);
        sprintf(response, "403 message format error\nUser %d doesn't exist\n", user_id);
    }
}

// Function to handle SELL command
void handle_sell(int socket, char *stock_symbol, double stock_amount, double price_per_stock, int user_id, char *response) {
    sqlite3_stmt *stmt;
    int rc;
    
    // Calculate total revenue
    double total_revenue = stock_amount * price_per_stock;
    
    // Check if user has enough stock
    const char *check_stock = "SELECT stock_balance FROM Stocks WHERE stock_symbol = ? AND user_id = ?;";
    rc = sqlite3_prepare_v2(db, check_stock, -1, &stmt, 0);
    sqlite3_bind_text(stmt, 1, stock_symbol, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, user_id);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        double current_stock = sqlite3_column_double(stmt, 0);
        sqlite3_finalize(stmt);
        
        // Check if user has enough stock to sell
        if (current_stock < stock_amount) {
            sprintf(response, "403 message format error\nNot enough %s stock balance. Current: %.2f, Requested: %.2f\n", 
                    stock_symbol, current_stock, stock_amount);
            return;
        }
        
        // Update stock balance
        const char *update_stock = "UPDATE Stocks SET stock_balance = stock_balance - ? WHERE stock_symbol = ? AND user_id = ?;";
        rc = sqlite3_prepare_v2(db, update_stock, -1, &stmt, 0);
        sqlite3_bind_double(stmt, 1, stock_amount);
        sqlite3_bind_text(stmt, 2, stock_symbol, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, user_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        // Add to user's balance
        const char *update_balance = "UPDATE Users SET usd_balance = usd_balance + ? WHERE ID = ?;";
        rc = sqlite3_prepare_v2(db, update_balance, -1, &stmt, 0);
        sqlite3_bind_double(stmt, 1, total_revenue);
        sqlite3_bind_int(stmt, 2, user_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        
        // Get updated balances
        const char *get_balance = "SELECT usd_balance FROM Users WHERE ID = ?;";
        rc = sqlite3_prepare_v2(db, get_balance, -1, &stmt, 0);
        sqlite3_bind_int(stmt, 1, user_id);
        
        double new_usd_balance = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            new_usd_balance = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);
        
        double new_stock_balance = current_stock - stock_amount;
        sprintf(response, "200 OK\nSOLD: New balance: %.2f %s. USD $%.2f\n", 
                new_stock_balance, stock_symbol, new_usd_balance);
    } else {
        sqlite3_finalize(stmt);
        sprintf(response, "403 message format error\nNo %s stock found for user %d\n", stock_symbol, user_id);
    }
}

// Function to handle LIST command
void handle_list(int socket, int user_id, char *response) {
    sqlite3_stmt *stmt;
    int rc;
    
    const char *list_query = "SELECT ID, stock_symbol, stock_balance FROM Stocks WHERE user_id = ?;";
    rc = sqlite3_prepare_v2(db, list_query, -1, &stmt, 0);
    sqlite3_bind_int(stmt, 1, user_id);
    
    sprintf(response, "200 OK\nThe list of records in the Stocks database for user %d:\n", user_id);
    
    int has_records = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        has_records = 1;
        int id = sqlite3_column_int(stmt, 0);
        const char *symbol = (const char*)sqlite3_column_text(stmt, 1);
        double balance = sqlite3_column_double(stmt, 2);
        
        char line[256];
        sprintf(line, "%d %s %.2f %d\n", id, symbol, balance, user_id);
        strcat(response, line);
    }
    
    if (!has_records) {
        strcat(response, "No stocks found for this user\n");
    }
    
    sqlite3_finalize(stmt);
}

// Function to handle BALANCE command
void handle_balance(int socket, int user_id, char *response) {
    sqlite3_stmt *stmt;
    int rc;
    
    const char *balance_query = "SELECT first_name, last_name, usd_balance FROM Users WHERE ID = ?;";
    rc = sqlite3_prepare_v2(db, balance_query, -1, &stmt, 0);
    sqlite3_bind_int(stmt, 1, user_id);
    
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *first_name = (const char*)sqlite3_column_text(stmt, 0);
        const char *last_name = (const char*)sqlite3_column_text(stmt, 1);
        double balance = sqlite3_column_double(stmt, 2);
        
        sprintf(response, "200 OK\nBalance for user %s %s: $%.2f\n", first_name, last_name, balance);
    } else {
        sprintf(response, "403 message format error\nUser %d doesn't exist\n", user_id);
    }
    
    sqlite3_finalize(stmt);
}

// Function to parse and handle commands
void handle_command(int socket, char *buf) {
    char command[MAX_LINE];
    char response[MAX_LINE * 4];
    
    // Remove trailing newline/whitespace
    char *newline = strchr(buf, '\n');
    if (newline) *newline = '\0';
    newline = strchr(buf, '\r');
    if (newline) *newline = '\0';
    
    // Print received message
    printf("Received: %s\n", buf);
    fflush(stdout);
    
    // Parse the command
    sscanf(buf, "%s", command);
    
    if (strcmp(command, "BUY") == 0) {
        char stock_symbol[10];
        double stock_amount, price_per_stock;
        int user_id;
        
        if (sscanf(buf, "BUY %s %lf %lf %d", stock_symbol, &stock_amount, &price_per_stock, &user_id) == 4) {
            handle_buy(socket, stock_symbol, stock_amount, price_per_stock, user_id, response);
        } else {
            sprintf(response, "403 message format error\nInvalid BUY command format\n");
        }
    }
    else if (strcmp(command, "SELL") == 0) {
        char stock_symbol[10];
        double stock_amount, price_per_stock;
        int user_id;
        
        if (sscanf(buf, "SELL %s %lf %lf %d", stock_symbol, &stock_amount, &price_per_stock, &user_id) == 4) {
            handle_sell(socket, stock_symbol, stock_amount, price_per_stock, user_id, response);
        } else {
            sprintf(response, "403 message format error\nInvalid SELL command format\n");
        }
    }
    else if (strcmp(command, "LIST") == 0) {
        int user_id = 1; // Default user
        // Try to parse user_id if provided
        sscanf(buf, "LIST %d", &user_id);
        handle_list(socket, user_id, response);
    }
    else if (strcmp(command, "BALANCE") == 0) {
        int user_id = 1; // Default user
        // Try to parse user_id if provided
        sscanf(buf, "BALANCE %d", &user_id);
        handle_balance(socket, user_id, response);
    }
    else if (strcmp(command, "QUIT") == 0) {
        sprintf(response, "200 OK\n");
    }
    else if (strcmp(command, "SHUTDOWN") == 0) {
        sprintf(response, "200 OK\n");
        send(socket, response, strlen(response), 0);
        fflush(stdout);
        close(socket);
        printf("Server shutting down...\n");
        sqlite3_close(db);
        exit(0);
    }
    else {
        sprintf(response, "400 invalid command\n");
    }
    
    // Send response to client
    send(socket, response, strlen(response), 0);
    fflush(stdout);
}

int main() {
    struct sockaddr_in sin;
    char buf[MAX_LINE];
    int buf_len;
    socklen_t addr_len;
    int s, new_s;

    // Initialize database
    if (init_database() != 0) {
        fprintf(stderr, "Failed to initialize database\n");
        exit(1);
    }
    
    printf("Database initialized successfully\n");
    printf("Server starting on port %d...\n", SERVER_PORT);

    /* build address data structure */
    bzero((char*)&sin, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(SERVER_PORT);

    /* setup passive open */
    if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
        perror("simplex-talk: socket");
        exit(1);
    }
    
    // Set socket option to reuse address
    int opt = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        exit(1);
    }
    
    if ((bind(s, (struct sockaddr*)&sin, sizeof(sin))) < 0) {
        perror("simplex-talk: bind");
        exit(1);
    }
    listen(s, MAX_PENDING);
    
    printf("Server listening for connections...\n");

    /* wait for connection, then receive and handle commands */
    while (1) {
        addr_len = sizeof(sin);
        if ((new_s = accept(s, (struct sockaddr*)&sin, &addr_len)) < 0) {
            perror("simplex-talk: accept");
            continue;
        }
        
        printf("Client connected\n");

        while ((buf_len = (int)recv(new_s, buf, sizeof(buf), 0)) > 0) {
            buf[buf_len] = '\0'; // Ensure null termination
            handle_command(new_s, buf);
            
            // Check if it was a QUIT command
            if (strncmp(buf, "QUIT", 4) == 0) {
                break;
            }
        }

        close(new_s);
        printf("Client disconnected\n");
    }
    
    // Close database (this won't be reached unless we add a proper shutdown mechanism)
    sqlite3_close(db);
    close(s);
    
    return 0;
}