#ifndef DIALOGUE_H
#define DIALOGUE_H

/*
 * This file simulates a JSON dialogue database.
 * It defines the structure for conversations, choices, and quest triggers.
 */

// --- Enums for Game State ---
enum GameMode {
    MODE_PLAYING,
    MODE_DIALOGUE
};

// --- Enums for Quest Logic ---
#define JESSE_QUEST_KITS_REQUIRED 3

enum QuestState {
    QUEST_NOT_STARTED,
    QUEST_ACTIVE,
    QUEST_COMPLETE
};

// --- Dialogue Data Structures ---

// Represents a single player choice
typedef struct {
    char text[100];     // The text shown for the choice
    int next_node;      // The ID of the node this choice leads to
    int quest_trigger;  // 0=none, 1=accept_quest, 2=complete_quest
} DialogueOption;

// Represents one "screen" of dialogue
typedef struct {
    int id;
    char npc_text[200];
    int num_options;
    DialogueOption options[4]; // Max 4 choices
} DialogueNode;


// --- Jesse's Complete Dialogue Tree ---
// We define all dialogue nodes here.

// Node 0: Intro
DialogueNode jesse_node_0 = {
    0,
    "Oh, thank god! Someone else! I.. I heard it.. that *thing*... it's in these caves! I'm too scared to move.",
    3,
    {
        {"\"What thing?\"", 1, 0},
        {"\"I'm busy. Bye.\"", -1, 0}, // -1 = End conversation
        {"\"Stay here, I'll protect you.\"", 2, 0}
    }
};

// Node 1: "What thing?"
DialogueNode jesse_node_1 = {
    1,
    "That... plant... monster! It walks on two legs. I've been hiding here, I'm hurt... I dropped all my supplies. I'm so weak...",
    2,
    {
        {"\"I can help you.\"", 3, 0},
        {"\"Sorry, can't help.\"", -1, 0}
    }
};

// Node 2: "I'll protect you."
DialogueNode jesse_node_2 = {
    2,
    "You... you will? Oh, thank you! But I'm hurt... I can't move. I saw some health kits scattered around when I ran. I.. I think I need 3 of them.",
    2,
    {
        {"I'll get them for you. [ACCEPT QUEST]", 10, 1}, // 1 = Accept Quest
        {"Too much work. Bye.", -1, 0}
    }
};

// Node 3: "I can help you." (leads to quest)
DialogueNode jesse_node_3 = {
    3,
    "Really? I.. I saw some health kits when I was running. If you could find 3 of them, I think I'd have the strength to move.",
    2,
    {
        {"I'll find them for you. [ACCEPT QUEST]", 10, 1}, // 1 = Accept Quest
        {"Nevermind.", -1, 0}
    }
};


// --- Quest Active Nodes ---

// Node 10: Quest accepted
DialogueNode jesse_node_10 = {
    10,
    "Thank you, thank you! Please be careful. That *thing* is still out there...",
    1,
    {
        {"[Leave]", -1, 0}
    }
};

// Node 11: Quest in progress (player has < 3 kits)
DialogueNode jesse_node_11 = {
    11,
    "You're back! Do you have the kits? It says you've only found... some of them. Please, I need 3 to feel safe!",
    1,
    {
        {"I'm still looking.", -1, 0}
    }
};

// Node 12: Quest complete (player has 3 kits)
DialogueNode jesse_node_12 = {
    12,
    "You found them! All 3! You saved me! I... I have the strength to move now. I'll follow you. Please, let's get out of here!",
    1,
    {
        {"Follow me. [QUEST COMPLETE]", -1, 2} // 2 = Complete Quest
    }
};


// --- Quest Complete Nodes ---

// Node 20: Jesse is now following
DialogueNode jesse_node_20 = {
    20,
    "I'm right behind you. Let's find the exit!",
    1,
    {
        {"[Leave]", -1, 0}
    }
};


#endif // DIALOGUE_H