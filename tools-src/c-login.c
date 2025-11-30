#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ctz-set.h"

// --- UI Helpers (ANSI Colors) ---
#define CLR_RESET   "\033[0m"
#define CLR_CYAN    "\033[1;36m"
#define CLR_GREEN   "\033[1;32m"
#define CLR_RED     "\033[1;31m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_WHITE   "\033[1;37m"
#define CLR_MAGENTA "\033[1;35m"
#define CLR_BLUE    "\033[1;34m"

void clear_screen() {
    #ifdef _WIN32
        system("cls");
    #else
        system("clear");
    #endif
}

void print_header(const char* title) {
    clear_screen();
    printf(CLR_CYAN "========================================\n");
    printf("   %s\n", title);
    printf("========================================\n" CLR_RESET);
}

void pause_screen() {
    printf("\nPress Enter to continue...");
    while(getchar() != '\n');
    getchar();
}

void flush_input() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}

// --- Application State ---
SetConfig* db = NULL;
SetNode* current_user = NULL;

void init_app() {
    srand(time(NULL)); // Seed RNG for the chatbot
    
    // Load or Create the Database
    db = set_load("users.set");
    if (!db) {
        db = set_create("users.set");
        set_db_insert(db, "Users"); 
    }
    set_db_init(db);

    if (!set_query(db, "Chats")) {
        set_set_child(set_get_root(db), "Chats", SET_TYPE_ARRAY);
    }
}

void save_app() {
    if (set_db_commit(db) != 0) {
        printf(CLR_RED "Error: Failed to save database!\n" CLR_RESET);
    }
}

// --- Helper: Get User Node by Username ---
SetNode* find_user(const char* username) {
    SetNode* results = set_db_select(db, "Users", "username", DB_OP_EQ, username, 1, 0);
    if (set_node_size(results) > 0) {
        SetIterator* it = set_iter_create(results);
        set_iter_next(it);
        SetNode* user = set_iter_value(it);
        set_iter_free(it);
        return user; 
    }
    return NULL;
}

// --- Auth Logic ---

int register_user() {
    print_header("REGISTER NEW USER");
    
    char username[64];
    char password[64];

    printf("Username: "); scanf("%63s", username);
    
    if (find_user(username)) {
        printf(CLR_RED "\nError: Username already taken.\n" CLR_RESET);
        pause_screen();
        return 0;
    }

    printf("Password: "); scanf("%63s", password);

    SetNode* user = set_db_insert(db, "Users");
    
    set_node_set_string(set_set_child(user, "username", SET_TYPE_STRING), username);
    set_node_set_string(set_set_child(user, "password", SET_TYPE_STRING), password);
    set_node_set_string(set_set_child(user, "bio", SET_TYPE_STRING), "New user of Cortez Terminal.");

    SetNode* settings = set_set_child(user, "settings", SET_TYPE_MAP);
    set_node_set_string(set_set_child(settings, "theme", SET_TYPE_STRING), "Default Blue");
    set_node_set_bool(set_set_child(settings, "notifications", SET_TYPE_BOOL), 1);

    set_set_child(user, "friends", SET_TYPE_ARRAY);
    set_set_child(user, "requests_in", SET_TYPE_ARRAY);

    save_app();
    printf(CLR_GREEN "\nSuccess! User registered.\n" CLR_RESET);
    pause_screen();
    return 1;
}

int login_user() {
    print_header("LOGIN");
    char username[64];
    char password[64];

    printf("Username: "); scanf("%63s", username);
    printf("Password: "); scanf("%63s", password);

    SetNode* user_node = find_user(username);
    
    if (!user_node) {
        printf(CLR_RED "\nError: User not found.\n" CLR_RESET);
        pause_screen();
        return 0;
    }

    const char* stored_pass = set_node_string(set_get_child(user_node, "password"), "");
    
    if (strcmp(stored_pass, password) == 0) {
        current_user = user_node;
        printf(CLR_GREEN "\nLogin Successful!\n" CLR_RESET);
        pause_screen();
        return 1;
    }

    printf(CLR_RED "\nError: Invalid Password.\n" CLR_RESET);
    pause_screen();
    return 0;
}

// --- Chat Features & AI ---

void get_chat_id(const char* u1, const char* u2, char* out_buf) {
    if (strcmp(u1, u2) < 0) sprintf(out_buf, "%s:%s", u1, u2);
    else sprintf(out_buf, "%s:%s", u2, u1);
}

SetNode* find_or_create_chat_session(const char* friend_name) {
    const char* my_name = set_node_string(set_get_child(current_user, "username"), "");
    char chat_id[128];
    get_chat_id(my_name, friend_name, chat_id);

    SetNode* results = set_db_select(db, "Chats", "id", DB_OP_EQ, chat_id, 1, 0);
    if (set_node_size(results) > 0) {
        SetIterator* it = set_iter_create(results);
        set_iter_next(it);
        SetNode* session = set_iter_value(it);
        set_iter_free(it);
        return session;
    }

    SetNode* chats_root = set_query(db, "Chats");
    if (!chats_root) chats_root = set_set_child(set_get_root(db), "Chats", SET_TYPE_ARRAY);

    SetNode* session = set_array_push(chats_root, SET_TYPE_MAP);
    set_node_set_string(set_set_child(session, "id", SET_TYPE_STRING), chat_id);
    set_set_child(session, "messages", SET_TYPE_ARRAY);
    
    save_app();
    return session;
}

// The "Mimic" Bot Logic
void trigger_mimic_bot(SetNode* msgs, const char* friend_name) {
    // 1. Harvest the friend's vocabulary from ALL chats
    SetNode* all_chats = set_query(db, "Chats");
    const char* phrases[500];
    int p_count = 0;

    // Scan every chat session in the DB
    for(size_t i=0; i<set_node_size(all_chats); i++) {
        SetNode* c = set_get_at(all_chats, i);
        SetNode* m_list = set_get_child(c, "messages");
        
        // Scan messages in this session
        for(size_t k=0; k<set_node_size(m_list); k++) {
            SetNode* msg = set_get_at(m_list, k);
            const char* sender = set_node_string(set_get_child(msg, "from"), "");
            
            // If this message was written by our friend, remember it
            if (strcmp(sender, friend_name) == 0) {
                if (p_count < 500) {
                    phrases[p_count++] = set_node_string(set_get_child(msg, "text"), "...");
                }
            }
        }
    }

    // 2. Pick a reply
    const char* reply;
    if (p_count > 0) {
        // Pick a random thing they have said before
        reply = phrases[rand() % p_count];
    } else {
        // Default if they have never spoken
        const char* defaults[] = { "Hello?", "I'm busy.", "brb", "lol", "what's up?" };
        reply = defaults[rand() % 5];
    }

    // 3. Insert the bot message
    SetNode* bot_msg = set_array_push(msgs, SET_TYPE_MAP);
    set_node_set_string(set_set_child(bot_msg, "from", SET_TYPE_STRING), friend_name);
    set_node_set_string(set_set_child(bot_msg, "text", SET_TYPE_STRING), reply);
    
    // Tag it as AI so we know (optional, but good for debugging)
    set_node_set_bool(set_set_child(bot_msg, "is_bot", SET_TYPE_BOOL), 1);

    save_app();
}

void chat_screen(const char* friend_name) {
    SetNode* session = find_or_create_chat_session(friend_name);
    SetNode* msgs = set_get_child(session, "messages");
    const char* my_name = set_node_string(set_get_child(current_user, "username"), "");

    while (1) {
        clear_screen();
        printf(CLR_CYAN "CHAT WITH %s" CLR_RESET "\n", friend_name);
        printf("========================================\n");

        size_t count = set_node_size(msgs);
        size_t start = (count > 15) ? count - 15 : 0;

        if (count == 0) printf(CLR_WHITE "  (No messages yet. Say hi!)\n" CLR_RESET);

        for (size_t i = start; i < count; i++) {
            SetNode* m = set_get_at(msgs, i);
            const char* sender = set_node_string(set_get_child(m, "from"), "?");
            const char* text = set_node_string(set_get_child(m, "text"), "...");
            
            if (strcmp(sender, my_name) == 0) {
                printf(CLR_GREEN "You: %s\n" CLR_RESET, text);
            } else {
                printf(CLR_YELLOW "%s: %s\n" CLR_RESET, sender, text);
            }
        }
        printf("========================================\n");
        printf("[Type message], [/q back], [/bot to trigger AI]: ");

        char input[256];
        flush_input();
        if (!fgets(input, sizeof(input), stdin)) break;
        input[strcspn(input, "\n")] = 0;

        if (strcmp(input, "/q") == 0) break;
        if (strlen(input) == 0) continue;

        // Manual trigger to test AI, or just normal chat
        if (strcmp(input, "/bot") == 0) {
            trigger_mimic_bot(msgs, friend_name);
            continue; 
        }

        // Add User Message
        SetNode* new_msg = set_array_push(msgs, SET_TYPE_MAP);
        set_node_set_string(set_set_child(new_msg, "from", SET_TYPE_STRING), my_name);
        set_node_set_string(set_set_child(new_msg, "text", SET_TYPE_STRING), input);
        
        save_app();

        // AUTO-REPLY: In a real app, we wouldn't do this, but for this demo
        // let's make the bot reply immediately to simulate a "live" friend.
        // Comment this line out if you want to test manual chatting between two terminals.
        trigger_mimic_bot(msgs, friend_name);
    }
}

// --- Social Features ---

void send_friend_request(const char* target_name) {
    if (strcmp(target_name, set_node_string(set_get_child(current_user, "username"), "")) == 0) {
        printf(CLR_RED "You cannot add yourself!\n" CLR_RESET);
        return;
    }

    SetNode* target = find_user(target_name);
    if (!target) {
        printf(CLR_RED "User '%s' not found.\n" CLR_RESET, target_name);
        return;
    }

    SetNode* reqs = set_get_child(target, "requests_in");
    if (!reqs) reqs = set_set_child(target, "requests_in", SET_TYPE_ARRAY);

    for(size_t i=0; i<set_node_size(reqs); i++) {
        const char* r = set_node_string(set_get_at(reqs, i), "");
        // FIX: Ignore processed/deleted requests
        if (strcmp(r, "PROCESSED") == 0) continue; 
        
        if (strcmp(r, set_node_string(set_get_child(current_user, "username"), "")) == 0) {
            printf(CLR_YELLOW "Request already sent.\n" CLR_RESET);
            return;
        }
    }

    SetNode* new_req = set_array_push(reqs, SET_TYPE_STRING);
    set_node_set_string(new_req, set_node_string(set_get_child(current_user, "username"), ""));

    save_app();
    printf(CLR_GREEN "Friend request sent to %s!\n" CLR_RESET, target_name);
}

void view_friend_requests() {
    print_header("FRIEND REQUESTS");
    
    SetNode* reqs = set_get_child(current_user, "requests_in");
    size_t count = set_node_size(reqs);
    
    size_t display_map[100];
    int valid = 0;

    for (size_t i = 0; i < count; i++) {
        const char* sender_name = set_node_string(set_get_at(reqs, i), "Unknown");
        // FIX: Do not display PROCESSED requests
        if (strcmp(sender_name, "PROCESSED") == 0) continue;

        printf("[%d] Request from: " CLR_YELLOW "%s" CLR_RESET "\n", valid + 1, sender_name);
        display_map[valid] = i;
        valid++;
    }

    if (valid == 0) {
        printf("No pending requests.\n");
        pause_screen();
        return;
    }

    printf("\nEnter ID to accept (0 to cancel): ");
    int choice;
    if (scanf("%d", &choice) != 1 || choice == 0) return;

    if (choice > 0 && choice <= valid) {
        size_t real_idx = display_map[choice - 1];
        const char* sender_name = set_node_string(set_get_at(reqs, real_idx), "");
        
        SetNode* my_friends = set_get_child(current_user, "friends");
        if (!my_friends) my_friends = set_set_child(current_user, "friends", SET_TYPE_ARRAY);
        set_node_set_string(set_array_push(my_friends, SET_TYPE_STRING), sender_name);

        SetNode* sender = find_user(sender_name);
        if (sender) {
            SetNode* their_friends = set_get_child(sender, "friends");
            if (!their_friends) their_friends = set_set_child(sender, "friends", SET_TYPE_ARRAY);
            set_node_set_string(set_array_push(their_friends, SET_TYPE_STRING), 
                set_node_string(set_get_child(current_user, "username"), ""));
        }

        set_node_set_string(set_get_at(reqs, real_idx), "PROCESSED");
        
        save_app();
        printf(CLR_GREEN "You are now friends with %s!\n" CLR_RESET, sender_name);
    }
    pause_screen();
}

void friend_menu() {
    while (1) {
        print_header("YOUR FRIENDS");
        SetNode* friends = set_get_child(current_user, "friends");
        size_t count = set_node_size(friends);

        int valid_count = 0;
        size_t index_map[100];

        if (count == 0) printf("No friends yet. Go socialize!\n");

        for (size_t i = 0; i < count; i++) {
            const char* f = set_node_string(set_get_at(friends, i), "");
            if (strcmp(f, "PROCESSED") != 0) {
                printf("[%d] " CLR_GREEN "%s" CLR_RESET "\n", valid_count + 1, f);
                index_map[valid_count] = i;
                valid_count++;
            }
        }

        printf("\n[ID] to Chat, [0] to Back: ");
        int choice;
        if (scanf("%d", &choice) != 1) {
            flush_input(); 
            break;
        }
        
        if (choice == 0) break;

        if (choice > 0 && choice <= valid_count) {
            size_t real_index = index_map[choice - 1];
            const char* friend_name = set_node_string(set_get_at(friends, real_index), "");
            chat_screen(friend_name);
        }
    }
}

void browse_users() {
    SetNode* users = set_query(db, "Users");
    size_t total = set_node_size(users);
    size_t current_idx = 0;
    
    while(1) {
        print_header("BROWSE USERS");
        
        SetNode* u = set_get_at(users, current_idx);
        const char* name = set_node_string(set_get_child(u, "username"), "Unknown");
        const char* bio = set_node_string(set_get_child(u, "bio"), "No bio set.");
        
        printf("User %zu / %zu\n", current_idx + 1, total);
        printf("----------------------------------------\n");
        printf("Name: " CLR_YELLOW "%s" CLR_RESET "\n", name);
        printf("Bio:  \n" CLR_WHITE "%s" CLR_RESET "\n", bio);
        printf("----------------------------------------\n");
        
        printf("[N] Next  [P] Previous  [A] Add Friend  [Q] Quit\n");
        printf("Action: ");
        
        char cmd;
        scanf(" %c", &cmd);
        
        if (cmd == 'n' || cmd == 'N') {
            if (current_idx < total - 1) current_idx++;
        }
        else if (cmd == 'p' || cmd == 'P') {
            if (current_idx > 0) current_idx--;
        }
        else if (cmd == 'a' || cmd == 'A') {
            send_friend_request(name);
            pause_screen();
        }
        else if (cmd == 'q' || cmd == 'Q') {
            break;
        }
    }
}

void edit_bio() {
    print_header("EDIT BIO");
    printf("Type your bio below.\n");
    printf(CLR_YELLOW "Press ENTER twice (empty line) to save.\n" CLR_RESET);
    printf("> ");

    char full_bio[1024] = "";
    char line[256];

    flush_input(); 

    while (fgets(line, sizeof(line), stdin)) {
        if (strcmp(line, "\n") == 0) break;
        if (strlen(full_bio) + strlen(line) < sizeof(full_bio) - 1) {
            strcat(full_bio, line);
        } else {
            printf(CLR_RED "Bio limit reached.\n" CLR_RESET);
            break;
        }
        printf("> ");
    }

    size_t len = strlen(full_bio);
    if (len > 0 && full_bio[len-1] == '\n') full_bio[len-1] = '\0';
    
    set_node_set_string(set_set_child(current_user, "bio", SET_TYPE_STRING), full_bio);
    save_app();
    printf(CLR_GREEN "Bio updated!\n" CLR_RESET);
    pause_screen();
}

void dashboard() {
    while (1) {
        const char* name = set_node_string(set_get_child(current_user, "username"), "?");
        SetNode* reqs = set_get_child(current_user, "requests_in");
        size_t pending = 0;
        
        for(size_t i=0; i<set_node_size(reqs); i++) {
            if (strcmp(set_node_string(set_get_at(reqs, i), ""), "PROCESSED") != 0) pending++;
        }

        // Check for unread chat notifications
        int unread_chats = 0;
        SetNode* friends = set_get_child(current_user, "friends");
        for(size_t i=0; i<set_node_size(friends); i++) {
             const char* fname = set_node_string(set_get_at(friends, i), "");
             if(strcmp(fname, "PROCESSED")==0) continue;
             
             char cid[128];
             get_chat_id(name, fname, cid);
             SetNode* c_res = set_db_select(db, "Chats", "id", DB_OP_EQ, cid, 1, 0);
             
             if(set_node_size(c_res) > 0) {
                 // Get session
                 SetIterator* it = set_iter_create(c_res);
                 set_iter_next(it);
                 SetNode* sess = set_iter_value(it);
                 SetNode* msgs = set_get_child(sess, "messages");
                 
                 // Check last message
                 size_t mcount = set_node_size(msgs);
                 if(mcount > 0) {
                     SetNode* last = set_get_at(msgs, mcount-1);
                     const char* last_sender = set_node_string(set_get_child(last, "from"), "");
                     if(strcmp(last_sender, name) != 0) unread_chats = 1;
                 }
                 set_iter_free(it);
             }
        }

        print_header("DASHBOARD");
        printf("User: " CLR_YELLOW "%s" CLR_RESET "\n", name);
        if (pending > 0) printf(CLR_MAGENTA "You have %zu friend requests!\n" CLR_RESET, pending);
        
        printf("\n");
        printf("  [1] Browse Users\n");
        
        if (unread_chats)
            printf("  [2] Friends & Chat " CLR_RED "(!)" CLR_RESET "\n");
        else
            printf("  [2] Friends & Chat\n");

        printf("  [3] Friend Requests\n");
        printf("  [4] Edit Profile\n");
        printf("  [5] Logout\n");
        printf("\nSelect > ");

        int choice;
        if (scanf("%d", &choice) != 1) break;

        switch(choice) {
            case 1: browse_users(); break;
            case 2: friend_menu(); break;
            case 3: view_friend_requests(); break;
            case 4: edit_bio(); break;
            case 5: current_user = NULL; return;
        }
    }
}

int main() {
    init_app();
    while (1) {
        print_header("CORTEZ SOCIAL v5.0 (AI Edition)");
        printf(" [1] Login\n");
        printf(" [2] Register\n");
        printf(" [3] Exit\n");
        printf("\nSelect > ");
        int choice;
        if (scanf("%d", &choice) != 1) break;
        switch (choice) {
            case 1: if (login_user()) dashboard(); break;
            case 2: register_user(); break;
            case 3: set_free(db); return 0;
        }
    }
    return 0;
}