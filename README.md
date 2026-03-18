## Stock Trading System 

This project implements a multithreaded TCP-based stock trading system in C, utilizing sockets and SQLite to manage concurrent user sessions and transactions. Unlike the previous iteration, this version leverages Pthreads to allow the server to handle multiple simultaneous client connections. The server maintains user accounts, balances, and stock portfolios in an SQLite database (‘stocks.db’) and manages persistent sessions via a login system. The client connects to the server to issue commands, including LOGIN, BUY, SELL, LIST, BALANCE, WHO, and LOOKUP, ensuring that all financial and portfolio actions are authorized and thread-safe.<br/>

- Platform: Linux/Unix (Ubuntu, WSL, or macOS)<br/>
- Programming language: C<br/>
- Database: SQLite3 (Multithreaded Server-side storage)<br/>
- Communication: TCP/IP Sockets on Port 6506 (Multithreaded)<br/>


## Running Instructions or using the Makefile

We included a Makefile that runs the program, so you only have to open the server and client and start the transaction:<br/>
Commands by using the Makefile:<br/>

Terminal 1<br/>
make clean<br/>
make<br/>
./server<br/>

Terminal 2<br/>
./client 127.0.0.1<br/>

Commands without using the Makefile:<br/>

In terminal one: gcc server.cpp sqlite3.c -o server -lpthread -ldl<br/>
in terminal two: gcc client.cpp -o client -pthread<br/>
in terminal one: ./server<br/>
in terminal two: ./client 127.0.0.1<br/>
Then you should be in to test<br/>


## Reflection and Bugs 
The transition to a multithreaded architecture introduced several synchronization and stability hurdles, most notably database locking (SQLITE_BUSY) when concurrent threads attempted to write to the same file, which was resolved by implementing a mutex around critical database calls. We also encountered zombie threads and memory leaks due to incomplete thread termination, necessitating the use of pthread_detach to ensure resources were automatically reclaimed upon client disconnection. Furthermore, the initial version suffered from race conditions in the global client list and from command-parsing errors caused by inconsistent \r\n line endings across terminal environments, both of which were fixed by adding strict mutex locking and robust string sanitization.<br/>

## Screenshots Highlighting Our Validation

As a regular user:<br/>
<img width="419" height="237" alt="image" src="https://github.com/user-attachments/assets/e7925f45-6b16-4511-955d-7543bac60bdc" />

<img width="429" height="228" alt="image" src="https://github.com/user-attachments/assets/6111bf18-8cb5-4165-a950-c29f7c8bf5f3" />

<img width="431" height="122" alt="image" src="https://github.com/user-attachments/assets/c5f88370-7b35-4904-b32a-9c9e0bd2717c" />

<br/><br/>

As the Root:<br/>
<img width="433" height="144" alt="image" src="https://github.com/user-attachments/assets/13c8bccb-744e-4710-a676-7f8cff3541ce" />

<img width="407" height="175" alt="image" src="https://github.com/user-attachments/assets/8c35a5ab-86dd-47c2-b58b-85e8fd8b94c5" />

<img width="468" height="185" alt="image" src="https://github.com/user-attachments/assets/a2c32fe9-60b9-4bd9-aebe-b76d155d438b" />

<br/><br/>

For both permissions:<br/>
<img width="405" height="158" alt="image" src="https://github.com/user-attachments/assets/8e4fd4c5-0160-461b-a56d-605ce6b186a8" />

<br/><br/>

The server:<br/>
<img width="412" height="247" alt="image" src="https://github.com/user-attachments/assets/ac5eca17-f46e-4119-9762-c3cb8457414e" />










