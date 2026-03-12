#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include "sqlite3.h"

#define SERVER_PORT 6506
#define MAX_PENDING 5
#define MAX_LINE    256
#define MAX_RESPONSE 2048
#define MAX_CLIENTS 10

//Global database pointer
sqlite3 *db;

pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;
int server_shutdown = 0;

typedef struct
{
    int socket;
    int active;
    int logged_in;
    int user_id;
    int is_root;
    char user_name[50];
    char ip[INET_ADDRSTRLEN];
    pthread_t thread_id;
} ClientSession;

typedef struct
{
    int client_index;
} ThreadArg;

ClientSession clients[MAX_CLIENTS];

//reset a client slot to its default empty state
void reset_client(int i)
{
    clients[i].socket = -1;
    clients[i].active = 0;
    clients[i].logged_in = 0;
    clients[i].user_id = 0;
    clients[i].is_root = 0;
    clients[i].user_name[0] = '\0';
    clients[i].ip[0] = '\0';
}

//set socket to nonblocking mode
int set_nonblocking(int sock)
{
    int opts;

    if((opts = fcntl(sock, F_GETFL)) < 0)
    {
        perror("fcntl F_GETFL");
        return -1;
    }

    opts = opts | O_NONBLOCK;

    if (fcntl(sock, F_SETFL, opts) < 0)
    {
        perror("fcntl F_SETFL");
        return -1;
    }

    return 0;
}

//function to initilaize the database and create tables
int init_database()
{
    char *err_msg = 0;
    int rc;

    //open database (creates if it doesnt exist)
    rc = sqlite3_open("stocks.db", &db);
    if(rc != SQLITE_OK)
    {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    //create user table
    const char *sql_users =
        "CREATE TABLE IF NOT EXISTS Users ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
        "user_name TEXT NOT NULL UNIQUE, "
        "password TEXT NOT NULL, "
        "usd_balance DOUBLE NOT NULL);";
    
    rc = sqlite3_exec(db, sql_users, 0, 0, &err_msg);
    if(rc != SQLITE_OK)
    {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 1;
    }

    //create stocks table
    const char *sql_stocks =
        "CREATE TABLE IF NOT EXISTS Stocks ("
        "ID INTEGER PRIMARY KEY AUTOINCREMENT, "
        "stock_symbol VARCHAR(4) NOT NULL, "
        "stock_name VARCHAR(20) NOT NULL, "
        "stock_balance DOUBLE, "
        "user_id INTEGER, "
        "FOREIGN KEY (user_id) REFERENCES Users (ID));";
    
    rc = sqlite3_exec(db, sql_stocks, 0, 0, &err_msg);
    if(rc != SQLITE_OK)
    {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 1;
    }

    //check if there are any users
    sqlite3_stmt *stmt;
    const char *check_users = "SELECT COUNT(*) FROM Users;";
    rc = sqlite3_prepare_v2(db, check_users, -1, &stmt, 0);

    if(rc != SQLITE_OK)
    {
        fprintf(stderr, "Prepare faild: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);

        if (count == 0)
        {
            /* insert default users: Root, Mary, John, Moe */
            const char *insert_user =
                "INSERT INTO Users (user_name, password, usd_balance) VALUES "
                "('Root', 'Root01', 100.0),"
                "('Mary', 'Mary01', 100.0),"
                "('John', 'John01', 100.0),"
                "('Moe', 'Moe01', 100.0);";

            rc = sqlite3_exec(db, insert_user, 0, 0, &err_msg);
            if (rc != SQLITE_OK)
            {
                fprintf(stderr, "SQL error: %s\n", err_msg);
                sqlite3_free(err_msg);
                return 1;
            }

            /* insert default stocks for John and Mary */
            const char *insert_stocks =
                "INSERT INTO Stocks (stock_symbol, stock_name, stock_balance, user_id) VALUES "
                "('MSFT', 'MSFT', 3.4, (SELECT ID FROM Users WHERE user_name='John')),"
                "('APPL', 'APPL', 5.0, (SELECT ID FROM Users WHERE user_name='John')),"
                "('U', 'U', 200.0, (SELECT ID FROM Users WHERE user_name='John')),"
                "('SP500', 'SP500', 100.32, (SELECT ID FROM Users WHERE user_name='Mary')),"
                "('MSFT', 'MSFT', 12.12, (SELECT ID FROM Users WHERE user_name='Mary'));";

            rc = sqlite3_exec(db, insert_stocks, 0, 0, &err_msg);
            if (rc != SQLITE_OK)
            {
                fprintf(stderr, "SQL warning: %s\n", err_msg);
                sqlite3_free(err_msg);
            }

            printf("Created default users: Root, Mary, John, Moe\n");
        }
    }
    else
    {
        sqlite3_finalize(stmt);
        return 1;
    }
    return 0;
}

/* LOGIN: verify credentials and mark client as logged in */
void handle_login(int client_index, char *user_name, char *password, char *response)
{
    sqlite3_stmt *stmt;
    int rc;

    pthread_mutex_lock(&global_mutex);

    /* check if this client is already logged in */
    if (clients[client_index].logged_in)
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 Already logged in as %s\n", clients[client_index].user_name);
        return;
    }

    const char *sql = "SELECT ID, user_name FROM Users WHERE user_name = ? AND password = ?;";
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK)
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "500 internal server error\n");
        return;
    }

    sqlite3_bind_text(stmt, 1, user_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, password, -1, SQLITE_TRANSIENT);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        clients[client_index].logged_in = 1;
        clients[client_index].user_id = sqlite3_column_int(stmt, 0);

        strncpy(clients[client_index].user_name,(const char *)sqlite3_column_text(stmt, 1), sizeof(clients[client_index].user_name) - 1);
        clients[client_index].user_name[sizeof(clients[client_index].user_name) - 1] = '\0';
        
        clients[client_index].is_root = (strcmp(clients[client_index].user_name, "Root") == 0);

        printf("User '%s' logged in from %s\n", clients[client_index].user_name, clients[client_index].ip);

        sprintf(response, "200 OK\n");
    }
    else
    {
        sprintf(response, "403 Wrong UserID or Password\n");
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&global_mutex);
}

//function to handle BUY command
void handle_buy(char *stock_symbol, double stock_amount, double price_per_stock, int user_id, char *response)
{
    sqlite3_stmt *stmt;
    int rc;

    // validate inputs
    if (stock_amount <= 0 || price_per_stock <= 0) 
    {
        sprintf(response, "403 message format error\nAmount and price must be > 0\n");
        return;
    }

    double total_cost = stock_amount * price_per_stock;

    pthread_mutex_lock(&global_mutex);


    // check user balance + name
    const char *check_balance = "SELECT usd_balance FROM Users WHERE ID = ?;";
    rc = sqlite3_prepare_v2(db, check_balance, -1, &stmt, 0);
    if (rc != SQLITE_OK) 
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nDatabase error\n");
        return;
    }

    sqlite3_bind_int(stmt, 1, user_id);

    if (sqlite3_step(stmt) != SQLITE_ROW) 
    {
        sqlite3_finalize(stmt);
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nUser %d doesn't exist\n", user_id);
        return;
    }

    double current_balance = sqlite3_column_double(stmt, 0);
    sqlite3_finalize(stmt);

    if (current_balance < total_cost) 
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nNot enough USD balance. Current: $%.2f, Required: $%.2f\n", current_balance, total_cost);
        return;
    }

    // deduct from user's USD
    const char *update_balance = "UPDATE Users SET usd_balance = usd_balance - ? WHERE ID = ?;";
    rc = sqlite3_prepare_v2(db, update_balance, -1, &stmt, 0);
    if (rc != SQLITE_OK) 
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nDatabase error\n");
        return;
    }
    sqlite3_bind_double(stmt, 1, total_cost);
    sqlite3_bind_int(stmt, 2, user_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // check if stock exists
    const char *check_stock = "SELECT stock_balance FROM Stocks WHERE stock_symbol = ? AND user_id = ?;";
    rc = sqlite3_prepare_v2(db, check_stock, -1, &stmt, 0);
    if (rc != SQLITE_OK) 
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nDatabase error\n");
        return;
    }

    sqlite3_bind_text(stmt, 1, stock_symbol, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, user_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) 
    {
        double current_stock = sqlite3_column_double(stmt, 0);
        sqlite3_finalize(stmt);

        const char *update_stock = "UPDATE Stocks SET stock_balance = stock_balance + ? " "WHERE stock_symbol = ? AND user_id = ?;";
        rc = sqlite3_prepare_v2(db, update_stock, -1, &stmt, 0);
        if (rc != SQLITE_OK) 
        {
            pthread_mutex_unlock(&global_mutex);
            sprintf(response, "403 message format error\nDatabase error\n");
            return;
        }

        sqlite3_bind_double(stmt, 1, stock_amount);
        sqlite3_bind_text(stmt, 2, stock_symbol, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, user_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        sprintf(response, "200 OK\nBOUGHT: New balance: %.2f %s. USD balance $%.2f\n",
                current_stock + stock_amount, stock_symbol, current_balance - total_cost);
    } 
        else 
        {
            sqlite3_finalize(stmt);

            const char *insert_stock = "INSERT INTO Stocks (stock_symbol, stock_name, stock_balance, user_id) " "VALUES (?, ?, ?, ?);";
            rc = sqlite3_prepare_v2(db, insert_stock, -1, &stmt, 0);
            if (rc != SQLITE_OK) 
            {
                pthread_mutex_unlock(&global_mutex);
                sprintf(response, "403 message format error\nDatabase error\n");
                return;
            }

        sqlite3_bind_text(stmt, 1, stock_symbol, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, stock_symbol, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 3, stock_amount);
        sqlite3_bind_int(stmt, 4, user_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        sprintf(response, "200 OK\nBOUGHT: New balance: %.2f %s. USD balance $%.2f\n", stock_amount, stock_symbol, current_balance - total_cost);
        }
            pthread_mutex_unlock(&global_mutex);

}


//function to handle sell command
/* SELL: add USD and remove stock shares */
void handle_sell(char *stock_symbol, double stock_amount, double price_per_stock,
                 int user_id, char *response)
{
    sqlite3_stmt *stmt;
    int rc;

    /* validate inputs */
    if (stock_amount <= 0 || price_per_stock <= 0)
    {
        sprintf(response, "403 message format error\nAmount and price must be > 0\n");
        return;
    }

    double total_revenue = stock_amount * price_per_stock;

    pthread_mutex_lock(&global_mutex);

    /* check current stock balance */
    const char *check_stock =
        "SELECT stock_balance FROM Stocks WHERE stock_symbol = ? AND user_id = ?;";
    rc = sqlite3_prepare_v2(db, check_stock, -1, &stmt, 0);
    if (rc != SQLITE_OK)
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nDatabase error\n");
        return;
    }
    sqlite3_bind_text(stmt, 1, stock_symbol, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, user_id);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        double current_stock = sqlite3_column_double(stmt, 0);
        sqlite3_finalize(stmt);

        /* check sufficient shares */
        if (current_stock < stock_amount)
        {
            pthread_mutex_unlock(&global_mutex);
            sprintf(response,
                    "403 message format error\nNot enough %s stock balance. "
                    "Current: %.2f, Requested: %.2f\n",
                    stock_symbol, current_stock, stock_amount);
            return;
        }

        /* deduct shares */
        const char *update_stock = "UPDATE Stocks SET stock_balance = stock_balance - ? ""WHERE stock_symbol = ? AND user_id = ?;";
        rc = sqlite3_prepare_v2(db, update_stock, -1, &stmt, 0);
        if (rc != SQLITE_OK)
        {
            pthread_mutex_unlock(&global_mutex);
            sprintf(response, "403 message format error\nDatabase error\n");
            return;
        }
        sqlite3_bind_double(stmt, 1, stock_amount);
        sqlite3_bind_text(stmt, 2, stock_symbol, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, user_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        /* add USD revenue */
        const char *update_balance =
            "UPDATE Users SET usd_balance = usd_balance + ? WHERE ID = ?;";
        rc = sqlite3_prepare_v2(db, update_balance, -1, &stmt, 0);
        if (rc != SQLITE_OK)
        {
            pthread_mutex_unlock(&global_mutex);
            sprintf(response, "403 message format error\nDatabase error\n");
            return;
        }
        sqlite3_bind_double(stmt, 1, total_revenue);
        sqlite3_bind_int(stmt, 2, user_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        /* get updated USD balance for response */
        const char *get_balance = "SELECT usd_balance FROM Users WHERE ID = ?;";
        rc = sqlite3_prepare_v2(db, get_balance, -1, &stmt, 0);
        if (rc != SQLITE_OK)
        {
            pthread_mutex_unlock(&global_mutex);
            sprintf(response, "403 message format error\nDatabase error\n");
            return;
        }
        sqlite3_bind_int(stmt, 1, user_id);

        double new_usd_balance = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW)
        {
            new_usd_balance = sqlite3_column_double(stmt, 0);
        }
        sqlite3_finalize(stmt);

        sprintf(response, "200 OK\nSOLD: New balance: %.2f %s. USD $%.2f\n", current_stock - stock_amount, stock_symbol, new_usd_balance);
    }
    else
    {
        sqlite3_finalize(stmt);
        sprintf(response, "403 message format error\nNo %s stock found for this user\n", stock_symbol);
    }

    pthread_mutex_unlock(&global_mutex);
}

//function to handle LIST command 
void handle_list(int client_index, char *response)
{
    sqlite3_stmt *stmt;
    int rc;
    int has_rows = 0;

    pthread_mutex_lock(&global_mutex);

    if (clients[client_index].is_root)
    {
        /* root sees all stocks with owner name */
        const char *list_query =
            "SELECT Stocks.ID, Stocks.stock_symbol, Stocks.stock_balance, Users.user_name "
            "FROM Stocks JOIN Users ON Stocks.user_id = Users.ID;";
        rc = sqlite3_prepare_v2(db, list_query, -1, &stmt, 0);
        if (rc != SQLITE_OK)
        {
            pthread_mutex_unlock(&global_mutex);
            sprintf(response, "403 message format error\nDatabase error\n");
            return;
        }

        sprintf(response, "200 OK\nThe list of records in the Stock database:\n");

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            has_rows = 1;
            int id = sqlite3_column_int(stmt, 0);
            const char *symbol = (const char *)sqlite3_column_text(stmt, 1);
            double balance = sqlite3_column_double(stmt, 2);
            const char *user = (const char *)sqlite3_column_text(stmt, 3);

            char line[128];
            sprintf(line, "%d %s %.2f %s\n", id, symbol, balance, user);
            if (strlen(response) + strlen(line) < MAX_RESPONSE)
                strcat(response, line);
        }
    }
    else
    {
        /* non-root sees only their own stocks */
        const char *list_query =
            "SELECT ID, stock_symbol, stock_balance FROM Stocks WHERE user_id = ?;";
        rc = sqlite3_prepare_v2(db, list_query, -1, &stmt, 0);
        if (rc != SQLITE_OK)
        {
            pthread_mutex_unlock(&global_mutex);
            sprintf(response, "403 message format error\nDatabase error\n");
            return;
        }
        sqlite3_bind_int(stmt, 1, clients[client_index].user_id);

        sprintf(response, "200 OK\nThe list of records in the Stock database for %s:\n",
                clients[client_index].user_name);

        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            has_rows = 1;
            int id = sqlite3_column_int(stmt, 0);
            const char *symbol = (const char *)sqlite3_column_text(stmt, 1);
            double balance = sqlite3_column_double(stmt, 2);

            char line[128];
            sprintf(line, "%d %s %.2f\n", id, symbol, balance);
            if (strlen(response) + strlen(line) < MAX_RESPONSE)
                strcat(response, line);
        }
    }

    if (!has_rows)
    {
        strcat(response, "No stocks found\n");
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&global_mutex);
}

// function to handle BALANCE command
/* BALANCE: show USD balance for the logged-in user */
void handle_balance(int client_index, char *response)
{
    sqlite3_stmt *stmt;
    int rc;

    pthread_mutex_lock(&global_mutex);

    const char *balance_query = "SELECT usd_balance FROM Users WHERE ID = ?;";
    rc = sqlite3_prepare_v2(db, balance_query, -1, &stmt, 0);
    if (rc != SQLITE_OK)
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nDatabase error\n");
        return;
    }
    sqlite3_bind_int(stmt, 1, clients[client_index].user_id);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        double balance = sqlite3_column_double(stmt, 0);
        sprintf(response, "200 OK\nBalance for user %s: $%.2f\n",
                clients[client_index].user_name, balance);
    }
    else
    {
        sprintf(response, "403 message format error\nUser doesn't exist\n");
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&global_mutex);
}

/* DEPOSIT: add USD to the logged-in user's account */
void handle_deposit(int client_index, double amount, char *response)
{
    sqlite3_stmt *stmt;
    int rc;

    if (amount <= 0)
    {
        sprintf(response, "403 message format error\nInvalid deposit amount\n");
        return;
    }

    pthread_mutex_lock(&global_mutex);

    const char *update_balance =
        "UPDATE Users SET usd_balance = usd_balance + ? WHERE ID = ?;";
    rc = sqlite3_prepare_v2(db, update_balance, -1, &stmt, 0);
    if (rc != SQLITE_OK)
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nDatabase error\n");
        return;
    }
    sqlite3_bind_double(stmt, 1, amount);
    sqlite3_bind_int(stmt, 2, clients[client_index].user_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* fetch updated balance to confirm */
    const char *get_balance = "SELECT usd_balance FROM Users WHERE ID = ?;";
    rc = sqlite3_prepare_v2(db, get_balance, -1, &stmt, 0);
    if (rc != SQLITE_OK)
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nDatabase error\n");
        return;
    }
    sqlite3_bind_int(stmt, 1, clients[client_index].user_id);

    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        double balance = sqlite3_column_double(stmt, 0);
        sprintf(response, "200 OK\nDeposit successful. New balance $%.2f\n", balance);    
    }
    else
    {
        sprintf(response, "403 message format error\nUser doesn't exist\n");
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&global_mutex);
}

/* LOOKUP: find stocks by partial or full ticker symbol for the logged-in user */
void handle_lookup(int client_index, char *ticker, char *response)
{
    sqlite3_stmt *stmt;
    int rc;
    char pattern[64];
    int count = 0;

    /* build LIKE pattern for partial matching */
    snprintf(pattern, sizeof(pattern), "%%%s%%", ticker);

    pthread_mutex_lock(&global_mutex);

    const char *lookup_query =
        "SELECT stock_symbol, stock_balance FROM Stocks "
        "WHERE user_id = ? AND stock_symbol LIKE ?;";

    /* first pass: count matches */
    rc = sqlite3_prepare_v2(db, lookup_query, -1, &stmt, 0);
    if (rc != SQLITE_OK)
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nDatabase error\n");
        return;
    }
    sqlite3_bind_int(stmt, 1, clients[client_index].user_id);
    sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0)
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "404 Your search did not match any records.\n");
        return;
    }

    sprintf(response, "200 OK\nFound %d match\n", count);

    /* second pass: fetch the actual rows */
    rc = sqlite3_prepare_v2(db, lookup_query, -1, &stmt, 0);
    if (rc != SQLITE_OK)
    {
        pthread_mutex_unlock(&global_mutex);
        sprintf(response, "403 message format error\nDatabase error\n");
        return;
    }
    sqlite3_bind_int(stmt, 1, clients[client_index].user_id);
    sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW)
    {
        const char *symbol = (const char *)sqlite3_column_text(stmt, 0);
        double balance = sqlite3_column_double(stmt, 1);

        char line[128];
        sprintf(line, "%s %.2f\n", symbol, balance);
        if (strlen(response) + strlen(line) < MAX_RESPONSE)
            strcat(response, line);
    }

    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&global_mutex);
}

/* WHO: root-only command — list all active logged-in users with IP addresses */
void handle_who(int client_index, char *response)
{
    int i;

    if (!clients[client_index].is_root)
    {
        sprintf(response, "403 message format error\nOnly root user can use WHO\n");
        return;
    }

    pthread_mutex_lock(&global_mutex);

    sprintf(response, "200 OK\nThe list of the active users:\n");
    for (i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active && clients[i].logged_in)
        {
            char line[128];
            sprintf(line, "%s %s\n", clients[i].user_name, clients[i].ip);
            if (strlen(response) + strlen(line) < MAX_RESPONSE)
                strcat(response, line);
        }
    }

    pthread_mutex_unlock(&global_mutex);
}



//function to parse and handle commands
void handle_command(int client_index, char *buf, char *response)
{
    char command[MAX_LINE];
    char *newline;

    newline = strchr(buf, '\n');
    if (newline) *newline = '\0';
    newline = strchr(buf, '\r');
    if (newline) *newline = '\0';

    printf("Received from client[%d] (%s): %s\n",
           client_index, clients[client_index].ip, buf);

    if (sscanf(buf, "%s", command) != 1)
    {
        sprintf(response, "400 invalid command\n");
        return;
    }

    if (strcmp(command, "LOGIN") == 0)
    {
        char user_name[50], password[50];

        if (sscanf(buf, "LOGIN %49s %49s", user_name, password) == 2)
        {
            handle_login(client_index, user_name, password, response);
        }
        else
        {
            sprintf(response,
                    "403 message format error\nUsage: LOGIN <username> <password>\n");
        }
    }
    else if (strcmp(command, "QUIT") == 0)
    {
        sprintf(response, "200 OK\n");
    }
    else
    {
        if (!clients[client_index].logged_in)
        {
            sprintf(response, "403 Please login first\n");
            return;
        }

        if (strcmp(command, "BUY") == 0)
        {
            char stock_symbol[10];
            double stock_amount, price_per_stock;

            if (sscanf(buf, "BUY %9s %lf %lf",
                       stock_symbol, &stock_amount, &price_per_stock) == 3)
            {
                handle_buy(stock_symbol, stock_amount, price_per_stock,
                           clients[client_index].user_id, response);
            }
            else
            {
                sprintf(response,
                        "403 message format error\nUsage: BUY <symbol> <amount> <price>\n");
            }
        }
        else if (strcmp(command, "SELL") == 0)
        {
            char stock_symbol[10];
            double stock_amount, price_per_stock;

            if (sscanf(buf, "SELL %9s %lf %lf",
                       stock_symbol, &stock_amount, &price_per_stock) == 3)
            {
                handle_sell(stock_symbol, stock_amount, price_per_stock,
                            clients[client_index].user_id, response);
            }
            else
            {
                sprintf(response,
                        "403 message format error\nUsage: SELL <symbol> <amount> <price>\n");
            }
        }
        else if (strcmp(command, "LIST") == 0)
        {
            handle_list(client_index, response);
        }
        else if (strcmp(command, "BALANCE") == 0)
        {
            handle_balance(client_index, response);
        }
        else if (strcmp(command, "DEPOSIT") == 0)
        {
            double amount;

            if (sscanf(buf, "DEPOSIT %lf", &amount) == 1)
            {
                handle_deposit(client_index, amount, response);
            }
            else
            {
                sprintf(response,"403 message format error\nUsage: DEPOSIT <amount>\n");
            }
        }
        else if (strcmp(command, "LOOKUP") == 0)
        {
            char ticker[20];

            if (sscanf(buf, "LOOKUP %19s", ticker) == 1)
            {
                handle_lookup(client_index, ticker, response);
            }
            else
            {
                sprintf(response,"403 message format error\nUsage: LOOKUP <ticker>\n");
            }
        }
        else if (strcmp(command, "WHO") == 0)
        {
            handle_who(client_index, response);
        }
        else if (strcmp(command, "LOGOUT") == 0)
        {
            printf("User '%s' logged out from %s\n", clients[client_index].user_name, clients[client_index].ip);

            pthread_mutex_lock(&global_mutex);
            clients[client_index].logged_in = 0;
            clients[client_index].user_id = 0;
            clients[client_index].is_root = 0;
            clients[client_index].user_name[0] = '\0';
            pthread_mutex_unlock(&global_mutex);

            sprintf(response, "200 OK\n");
        }
        else if (strcmp(command, "SHUTDOWN") == 0)
        {
            if (!clients[client_index].is_root)
            {
                sprintf(response,
                        "403 message format error\nOnly root user can shut down the server\n");
            }
            else
            {
                printf("SHUTDOWN command received from root user '%s'\n",
                       clients[client_index].user_name);
                server_shutdown = 1;
                sprintf(response, "200 OK\nServer shutting down\n");
            }
        }
        else
        {
            sprintf(response, "400 invalid command\n");
        }
    }
}
/* thread function: handles all commands for a fully logged-in client */
void *client_thread(void *arg)
{
    ThreadArg *thread_arg = (ThreadArg *)arg;
    int client_index = thread_arg->client_index;
    char buf[MAX_LINE];
    char response[MAX_RESPONSE];
    int len;

    free(thread_arg);

    while (!server_shutdown)
    {
        memset(buf, 0, sizeof(buf));
        len = recv(clients[client_index].socket, buf, sizeof(buf) - 1, 0);

        if (len <= 0)
        {
            printf("Client[%d] (%s) disconnected\n",
                   client_index, clients[client_index].ip);
            break;
        }

        buf[len] = '\0';
        memset(response, 0, sizeof(response));

        handle_command(client_index, buf, response);

        if (send(clients[client_index].socket, response, strlen(response), 0) < 0)
        {
            perror("send");
            break;
        }

        /* exit the thread if client logged out, quit, or server is shutting down */
        if (strncmp(buf, "LOGOUT", 6) == 0 ||
            strncmp(buf, "QUIT",   4) == 0 ||
            server_shutdown)
        {
            break;
        }
    }

    close(clients[client_index].socket);

    pthread_mutex_lock(&global_mutex);
    reset_client(client_index);
    pthread_mutex_unlock(&global_mutex);

    return NULL;
}

/* accept a new connection and store it in the next available client slot */
void add_client(int new_s, struct sockaddr_in client_addr)
{
    int i;
    int found_slot = -1;

    for (i = 0; i < MAX_CLIENTS; i++)
    {
        if (!clients[i].active)
        {
            found_slot = i;
            break;
        }
    }

    if (found_slot == -1)
    {
        /* no available slot — reject the connection */
        const char *msg = "Server full. Try again later.\n";
        send(new_s, msg, strlen(msg), 0);
        close(new_s);
        printf("Rejected connection: server full\n");
    }
    else
    {
        clients[found_slot].socket = new_s;
        clients[found_slot].active = 1;
        clients[found_slot].logged_in = 0;
        strncpy(clients[found_slot].ip, inet_ntoa(client_addr.sin_addr), INET_ADDRSTRLEN - 1);
        clients[found_slot].ip[INET_ADDRSTRLEN - 1] = '\0';
        printf("Client connected from %s (slot %d)\n", clients[found_slot].ip, found_slot);
    }
}

/*
 * check_clients: called in the select() loop to handle pre-login clients.
 * Once a client successfully logs in, spawn a dedicated thread for them.
 */
void check_clients(fd_set *read_set)
{
    int i;

    for (i = 0; i < MAX_CLIENTS; i++)
    {
        /* only process active, not-yet-threaded (pre-login) clients */
        if (clients[i].active && !clients[i].logged_in &&
            FD_ISSET(clients[i].socket, read_set))
        {
            char buf[MAX_LINE];
            char response[MAX_RESPONSE];

            int len = recv(clients[i].socket, buf, sizeof(buf) - 1, 0);

            if (len < 0)
            {
                if (errno == EWOULDBLOCK || errno == EAGAIN)
                    continue;

                close(clients[i].socket);
                reset_client(i);
                continue;
            }

            if (len == 0)
            {
                /* client disconnected before logging in */
                close(clients[i].socket);
                reset_client(i);
                continue;
            }

            buf[len] = '\0';
            memset(response, 0, sizeof(response));

            handle_command(i, buf, response);

            if (send(clients[i].socket, response, strlen(response), 0) < 0)
            {
                perror("send");
                close(clients[i].socket);
                reset_client(i);
                continue;
            }

            if (strncmp(buf, "QUIT", 4) == 0)
            {
                /* client quit without ever logging in */
                close(clients[i].socket);
                reset_client(i);
            }
            else if (clients[i].logged_in)
            {
                /* login succeeded — hand off to a dedicated thread */
                ThreadArg *arg = malloc(sizeof(ThreadArg));
                if (arg == NULL)
                {
                    close(clients[i].socket);
                    reset_client(i);
                    continue;
                }

                arg->client_index = i;

                if (pthread_create(&clients[i].thread_id, NULL,
                                   client_thread, arg) != 0)
                {
                    perror("pthread_create");
                    free(arg);
                    close(clients[i].socket);
                    reset_client(i);
                    continue;
                }

                /* detach so resources are freed automatically on exit */
                pthread_detach(clients[i].thread_id);
            }
        }
    }
}

int main()
{
    struct sockaddr_in sin, client_addr;
    socklen_t addr_len;
    int s, new_s;
    fd_set read_set;
    int maxfd;
    int opt = 1;
    int i;

    /* initialize all client slots to empty */
    for (i = 0; i < MAX_CLIENTS; i++)
    {
        reset_client(i);
    }

    if (init_database() != 0)
    {
        fprintf(stderr, "Failed to initialize database\n");
        exit(1);
    }

    printf("Database initialized successfully\n");
    printf("Server starting on port %d...\n", SERVER_PORT);

    bzero((char *)&sin, sizeof(sin));
    sin.sin_family      = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port        = htons(SERVER_PORT);

    if ((s = socket(PF_INET, SOCK_STREAM, 0)) < 0)
    {
        perror("socket");
        exit(1);
    }

    /* allow immediate reuse of the port after restart */
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt");
        close(s);
        exit(1);
    }

    /* make the listening socket non-blocking so select() works properly */
    if (set_nonblocking(s) < 0)
    {
        close(s);
        exit(1);
    }

    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) < 0)
    {
        perror("bind");
        close(s);
        exit(1);
    }

    if (listen(s, MAX_PENDING) < 0)
    {
        perror("listen");
        close(s);
        exit(1);
    }

    printf("Server listening for connections...\n");

    /* main select() loop: accept new connections and handle pre-login clients */
    while (!server_shutdown)
    {
        FD_ZERO(&read_set);
        FD_SET(s, &read_set);
        maxfd = s;

        /* add all active pre-login client sockets to the read set */
        for (i = 0; i < MAX_CLIENTS; i++)
        {
            if (clients[i].active && !clients[i].logged_in)
            {
                FD_SET(clients[i].socket, &read_set);
                if (clients[i].socket > maxfd)
                    maxfd = clients[i].socket;
            }
        }

        if (select(maxfd + 1, &read_set, NULL, NULL, NULL) < 0)
        {
            if (errno == EINTR)
                continue; /* interrupted by a signal — retry */
            perror("select");
            continue;
        }

        /* handle new incoming connection */
        if (FD_ISSET(s, &read_set))
        {
            addr_len = sizeof(client_addr);
            new_s = accept(s, (struct sockaddr *)&client_addr, &addr_len);

            if (new_s >= 0)
            {
                add_client(new_s, client_addr);
            }
            else if (errno != EWOULDBLOCK && errno != EAGAIN)
            {
                perror("accept");
            }
        }

        /* dispatch commands from pre-login clients */
        check_clients(&read_set);
    }

    printf("Server shutting down — notifying all connected clients...\n");

    /* notify and close all remaining active clients */
    for (i = 0; i < MAX_CLIENTS; i++)
    {
        if (clients[i].active)
        {
            const char *shutdown_msg = "200 OK\nServer shutting down\n";
            send(clients[i].socket, shutdown_msg, strlen(shutdown_msg), 0);
            close(clients[i].socket);
            reset_client(i);
        }
    }

    close(s);
    sqlite3_close(db);
    printf("Server terminated.\n");

    return 0;
}