# What both apps share: starting up from the flags and config (Runtime), the
# hooks a frontend gives an agent, transcript entries and replaying them,
# slash commands, greetings, durations, folder listings and task worktrees.
add_library(smith_frontend STATIC
  Activity.cpp
  Commands.cpp
  Duration.cpp
  Entry.cpp
  Folders.cpp
  Greetings.cpp
  Replay.cpp
  Runtime.cpp
  Worktrees.cpp
)
target_link_libraries(smith_frontend PUBLIC smith_core)
