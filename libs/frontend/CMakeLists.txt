# What both apps share: starting up from the flags and config (Runtime), the
# hooks a frontend gives an agent, transcript entries and replaying them,
# running turns and asking the user (Turns, Prompts), slash commands,
# greetings, durations, folder and file listings and task worktrees.
add_library(smith_frontend STATIC
  Activity.cpp
  Commands.cpp
  Controller.cpp
  Duration.cpp
  Entry.cpp
  FileIndex.cpp
  Folders.cpp
  Greetings.cpp
  Prompts.cpp
  Replay.cpp
  Runtime.cpp
  Turns.cpp
  Worktrees.cpp
)
target_link_libraries(smith_frontend PUBLIC smith_core)
