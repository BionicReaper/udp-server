/*
 * Input Server for Windows
 * 
 * This server captures keyboard input on Windows and sends it to a client
 * (typically WSL running the game client) over TCP on port 53850.
 * 
 * Protocol: Each message is 2 bytes: [keycode][state]
 * - keycode: 'W', 'A', 'S', 'D', 'L' (left), 'R' (right), ' ' (space), 'U' (up), 'N' (down)
 * - state: 0 = release, 1 = press
 * 
 * Compile on Windows with:
 *   gcc -o input_server.exe input_server.c -lws2_32
 * 
 * Or with MSVC:
 *   cl input_server.c ws2_32.lib user32.lib
 */

#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")

#define INPUT_SERVER_PORT 53850
#define MAX_CLIENTS 4

// Global state
static SOCKET listen_sock = INVALID_SOCKET;
static SOCKET client_socks[MAX_CLIENTS];
static int client_count = 0;
static CRITICAL_SECTION cs;
static volatile int server_running = 1;

// Track key states to detect transitions
static int key_states[256] = {0};

// Send input event to all connected clients
static void broadcast_input(unsigned char keycode, unsigned char state) {
    unsigned char msg[2] = {keycode, state};
    
    EnterCriticalSection(&cs);
    for (int i = 0; i < client_count; i++) {
        send(client_socks[i], (const char*)msg, 2, 0);
    }
    LeaveCriticalSection(&cs);
}

// Map Windows virtual key codes to our protocol
static unsigned char vk_to_keycode(int vk) {
    switch (vk) {
        case 'W': return 'W';
        case 'A': return 'A';
        case 'S': return 'S';
        case 'D': return 'D';
        case VK_LEFT: return 'L';
        case VK_RIGHT: return 'R';
        case VK_SPACE: return ' ';
        case VK_UP: return 'U';
        case VK_DOWN: return 'N';
        default: return 0;
    }
}

// Low-level keyboard hook callback
static LRESULT CALLBACK keyboard_hook_proc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT*)lParam;
        int vk = kb->vkCode;
        unsigned char keycode = vk_to_keycode(vk);
        
        if (keycode != 0) {
            int pressed = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
            int released = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
            
            if (pressed && !key_states[vk]) {
                key_states[vk] = 1;
                broadcast_input(keycode, 1);
            } else if (released && key_states[vk]) {
                key_states[vk] = 0;
                broadcast_input(keycode, 0);
            }
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// Thread to accept client connections
static DWORD WINAPI accept_thread(LPVOID param) {
    (void)param;
    
    while (server_running) {
        struct sockaddr_in client_addr;
        int addr_len = sizeof(client_addr);
        
        SOCKET client = accept(listen_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client == INVALID_SOCKET) {
            if (server_running) {
                Sleep(100);
            }
            continue;
        }
        
        EnterCriticalSection(&cs);
        if (client_count < MAX_CLIENTS) {
            client_socks[client_count++] = client;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
            printf("Client connected from %s (total: %d)\n", ip, client_count);
        } else {
            printf("Max clients reached, rejecting connection\n");
            closesocket(client);
        }
        LeaveCriticalSection(&cs);
    }
    
    return 0;
}

// Thread to check for disconnected clients
static DWORD WINAPI cleanup_thread(LPVOID param) {
    (void)param;
    
    while (server_running) {
        Sleep(1000);
        
        EnterCriticalSection(&cs);
        for (int i = client_count - 1; i >= 0; i--) {
            // Try to send a keepalive (empty or zero-op)
            char test = 0;
            int result = send(client_socks[i], &test, 0, 0);
            if (result == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAECONNRESET || err == WSAENOTCONN || err == WSAESHUTDOWN) {
                    printf("Client %d disconnected\n", i);
                    closesocket(client_socks[i]);
                    // Shift remaining clients
                    for (int j = i; j < client_count - 1; j++) {
                        client_socks[j] = client_socks[j + 1];
                    }
                    client_count--;
                }
            }
        }
        LeaveCriticalSection(&cs);
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("=== Windows Input Server ===\n");
    printf("Port: %d\n\n", INPUT_SERVER_PORT);
    
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
    
    InitializeCriticalSection(&cs);
    
    // Create listening socket
    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) {
        fprintf(stderr, "Socket creation failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }
    
    // Allow address reuse
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
    
    // Bind to localhost only (for WSL communication)
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    server_addr.sin_port = htons(INPUT_SERVER_PORT);
    
    if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "Bind failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }
    
    if (listen(listen_sock, MAX_CLIENTS) == SOCKET_ERROR) {
        fprintf(stderr, "Listen failed: %d\n", WSAGetLastError());
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }
    
    printf("Listening on 127.0.0.1:%d\n", INPUT_SERVER_PORT);
    printf("Press Ctrl+C to exit\n\n");
    printf("Keys captured: W, A, S, D, Arrow keys, Space\n\n");
    
    // Start accept thread
    HANDLE accept_handle = CreateThread(NULL, 0, accept_thread, NULL, 0, NULL);
    if (!accept_handle) {
        fprintf(stderr, "Failed to create accept thread\n");
        closesocket(listen_sock);
        WSACleanup();
        return 1;
    }
    
    // Start cleanup thread
    HANDLE cleanup_handle = CreateThread(NULL, 0, cleanup_thread, NULL, 0, NULL);
    if (!cleanup_handle) {
        fprintf(stderr, "Failed to create cleanup thread\n");
    }
    
    // Install keyboard hook
    HHOOK hook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboard_hook_proc, NULL, 0);
    if (!hook) {
        fprintf(stderr, "Failed to install keyboard hook: %lu\n", GetLastError());
        server_running = 0;
        closesocket(listen_sock);
        WaitForSingleObject(accept_handle, 1000);
        CloseHandle(accept_handle);
        WSACleanup();
        return 1;
    }
    
    printf("Keyboard hook installed. Capturing input...\n");
    
    // Message loop (required for low-level keyboard hook)
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    
    // Cleanup
    server_running = 0;
    UnhookWindowsHookEx(hook);
    closesocket(listen_sock);
    
    WaitForSingleObject(accept_handle, 1000);
    CloseHandle(accept_handle);
    
    if (cleanup_handle) {
        WaitForSingleObject(cleanup_handle, 1000);
        CloseHandle(cleanup_handle);
    }
    
    EnterCriticalSection(&cs);
    for (int i = 0; i < client_count; i++) {
        closesocket(client_socks[i]);
    }
    LeaveCriticalSection(&cs);
    
    DeleteCriticalSection(&cs);
    WSACleanup();
    
    printf("Server stopped.\n");
    return 0;
}

#else
// Non-Windows stub
#include <stdio.h>

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("This program is designed to run on Windows.\n");
    printf("It captures keyboard input and sends it to WSL over TCP.\n");
    printf("\nCompile on Windows with:\n");
    printf("  gcc -o input_server.exe input_server.c -lws2_32\n");
    printf("\nOr with MSVC:\n");
    printf("  cl input_server.c ws2_32.lib user32.lib\n");
    return 1;
}

#endif
