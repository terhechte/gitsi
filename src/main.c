#define _GNU_SOURCE
#include <git2.h>
#ifdef __APPLE__
#include <curses.h>
#else
#include <ncurses.h>
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ftw.h>
#include <signal.h>

#define MIN(a,b) (((a)<(b))?(a):(b))
#define MAX(a,b) (((a)>(b))?(a):(b))

/* Lightweight abstraction over colors */
enum GITSI_COLOR {
    GITSI_COLOR_INDEX = 1,
    GITSI_COLOR_UNTRACKED = 2,
    GITSI_COLOR_TITLE = 3,
    GITSI_COLOR_WORKSPACE = 4,
    GITSI_COLOR_VISUAL_SELECT = 5,
};

/* Files are one of these types */
enum GITSI_STATUS_TYPE {
    STATUS_TYPE_WORKSPACE,
    STATUS_TYPE_INDEX,
    STATUS_TYPE_UNTRACKED,
    STATUS_TYPE_CATEGORY
};

/* These are used to display on-screen help */
typedef struct gitsi_help_entry {
    const char *key;
    const char *name;
    const char *desc;
} gitsi_help_entry;

// test

/* A list of all the features to be used for the status bar and the help screen */
const gitsi_help_entry help_entries[] = {
    {.key = "j", .name = "down", .desc = "Go to the next line"},
    {.key = "k", .name = "up", .desc = "Go to the previous line"},
    {.key = "s", .name = "ACTION_A", .desc = "Add file or stage changes"},
    {.key = "u", .name = "ACTION_B", .desc = "Unstage changes or delete file"},
    {.key = "/", .name = "filter", .desc = "Filter the list of files"},
    {.key = "q", .name = "quit", .desc = "Quit the program"},
    
    {.key = "d", .name = "diff", .desc = "Run `git diff` on the selected file"},
    {.key = "r", .name = "reload", .desc = "Reload the repository"},
    {.key = "i", .name = "add -p", .desc = "Run git interactive add on the selected file"},
    {.key = "e", .name = "edit", .desc = "Open the file in vim"},
    {.key = "c", .name = "commit", .desc = "Run `git commit`"},
    {.key = "C-d", .name = "jump down", .desc = "Jump half a screen down"},
    {.key = "C-u", .name = "jump up", .desc = "Jump half a screen up"},
    {.key = "!", .name = "go index", .desc = "Jump to the index [Shift 1]"},
    {.key = "@", .name = "go workspace", .desc = "Jump to the workspace [Shift 2]"},
    {.key = "#", .name = "go untracked", .desc = "Jump to the untracked [Shift 3]"},
    {.key = "G", .name = "bottom", .desc = "Jump to the bottom of the list"},
    {.key = "g", .name = "top", .desc = "Jump to the top of the list"},
    {.key = "m", .name = "mark", .desc = "Mark / Unmark the selected file"},
    {.key = "M", .name = "mark section", .desc = "Mark / Unmark all files in section"},
    {.key = "V", .name = "visual mark mode", .desc = "Toggle Visual Mark mode to mark files by moving. ESC cancels"},
    {.key = "C", .name = "amend", .desc = "Run `git commit --amend`"},
    {.key = "p", .name = "push", .desc = "Run `git push`"},
    {.key = "P", .name = "push -u", .desc = "Run `git push -u`"},
    {.key = "S", .name = "s action on marked", .desc = "Perform the add/stage action on all marked files"},
    {.key = "U", .name = "u action on marked", .desc = "Perform the unstage/delete action on all marked files"},
    {.key = "x", .name = "Reset", .desc = "Remove / Reset all changes this file has. Like `git checkout -- file`"},
    {.key = ":", .name = "Command", .desc = "Run git command. I.e. :log for git log"},
};

#define help_entries_length (sizeof (help_entries) / sizeof (const gitsi_help_entry))

/* Each entry in the list is of this type */
typedef struct gitsi_status_entry {
    const char *filename;
    const char *description;
    enum GITSI_STATUS_TYPE type;
    bool marked;
    git_status_t git_status;
} gitsi_status_entry;

#define MAX_INPUT_CHARS 512
#define MAX_NUMBER_STACK 8

// #define DEBUG 1

#if DEBUG
#define LOGFILE_NAME "/tmp/gitsi.log"
#endif

/* The context stores what the current UI looks like.
 - All the git status entries
 - The filtered entries
 - Which screen to display
 - If search is active
 - If visual mark mode is active
 etc */
typedef struct gitsi_context {
    // Screen state
    bool has_color;
    int max_x;
    int max_y;
    
    // Repositories State
    char *repo_dir;
    git_repository *repo;
    git_index *repo_index;
    
    // Entries state
    gitsi_status_entry **entries;
    size_t entry_count;
    
    // Search / Filter State
    bool is_search;
    char search_term[MAX_INPUT_CHARS];
    gitsi_status_entry **filtered_entries;
    size_t filtered_entry_count;

    // Command state
    char command_term[MAX_INPUT_CHARS];
    bool is_in_command_mode;
    
    // List state
    gitsi_status_entry *position;

    // UI State
    bool is_visual_mark_mode;
    bool is_in_help;
    char number_stack[MAX_NUMBER_STACK];
    int number_stack_count;
    
    // Log State
#if DEBUG
    FILE *logfile;
#endif
} gitsi_context;

/* All the possible keystrokes for navigation and actions are defiend here */
enum key_stroke {
    // Actions
    K_SLASH, K_Q, K_S, K_U, K_S_S, K_S_U, K_D, K_I, K_M, K_S_M, K_C, K_E, K_R,
    K_BACKSPACE, K_ESC, K_ENTER, K_YES, K_NO, K_H, K_S_V, K_S_C, K_X, K_P, K_S_P,
    // Navigation
    K_G, K_C_U, K_C_D, K_J, K_K, K_S_G, K_S_1, K_S_2, K_S_3,
    K_ARROW_LEFT, K_ARROW_RIGHT, K_ARROW_UP, K_ARROW_DOWN,
    K_COMMAND,
    K_HELP,
    K_OTHER
};

void gitsi_debug_str(gitsi_context *context, const char *format, ...);

// --------------------------------------------------
#pragma mark Helpers
// --------------------------------------------------

/* Translate what the user entered into one of our key constants */
enum key_stroke translate_key(gitsi_context *context, int ch) {
    if (ch == 10)return K_ENTER;
    if (ch == 259)return K_ARROW_UP;
    if (ch == 258)return K_ARROW_DOWN;
    if (ch == 260)return K_ARROW_LEFT;
    if (ch == 261)return K_ARROW_RIGHT;

    // This returns a pointer, but I could not figure out via the docs
    // whether the pointer needs to be freed. Valgrind laments that we're
    // leaking here, but in one document, it says:
    // http://pubs.opengroup.org/onlinepubs/7908799/xcurses/keyname.html
    // "The return value of keyname() and key_name() may point to a static area which is overwritten by a subsequent call to either of these functions."
    const char *key = keyname(ch);
#define CMP(arg) strcmp(key, arg) == 0
    if (CMP("/"))return K_SLASH;
    if (CMP("q"))return K_Q;
    if (CMP("j"))return K_J;
    if (CMP("k"))return K_K;
    if (CMP("r"))return K_R;

    if (CMP(":"))return K_COMMAND;
    
    if (CMP("s"))return K_S;
    if (CMP("u"))return K_U;

    if (CMP("?"))return K_HELP;
    
    if (CMP("S"))return K_S_S;
    if (CMP("U"))return K_S_U;
    if (CMP("m"))return K_M;
    if (CMP("M"))return K_S_M;
    if (CMP("V"))return K_S_V;
    if (CMP("c"))return K_C;
    if (CMP("C"))return K_S_C;
    if (CMP("x"))return K_X;
    if (CMP("h"))return K_H;
    if (CMP("p"))return K_P;
    if (CMP("P"))return K_S_P;
    
    if (CMP("d"))return K_D;
    if (CMP("e"))return K_E;
    if (CMP("g"))return K_G;
    if (CMP("i"))return K_I;
    if (CMP("!"))return K_S_1;
    if (CMP("@"))return K_S_2;
    if (CMP("#"))return K_S_3;
    
    if (CMP("Y"))return K_YES;
    if (CMP("N"))return K_NO;
    if (CMP("G"))return K_S_G;
    
    if (CMP("^U"))return K_C_U;
    if (CMP("^D"))return K_C_D;
    
    if (CMP("^?"))return K_BACKSPACE;
    if (CMP("^["))return K_ESC;
    if (CMP("^M"))return K_ENTER;

    gitsi_debug_str(context, "(%i)\n", ch);

    return K_OTHER;
}

/* Small abstraction to append debug strings to the /tmp/gitsi.log logfile */
void gitsi_debug_str(gitsi_context *context, const char *format, ...) {
#if DEBUG
    va_list args;
    va_start(args, format);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
    vfprintf(context->logfile, format, args);
#pragma clang diagnostic pop
    fflush(context->logfile);
    va_end(args);
#endif
}

/* Helper function to determine the file type of a filename */
enum file_type {
    FILE_TYPE_DIRECTORY,
    FILE_TYPE_FILE,
    FILE_TYPE_OTHER
} util_is_regular_file(const char *repo_dir, const char *filename) {
    // build the full path
    char *buffer;
    asprintf(&buffer, "%s/%s", repo_dir, filename);
    struct stat path_stat;
    stat(buffer, &path_stat);
    free(buffer);
    if (S_ISREG(path_stat.st_mode)) {
        return FILE_TYPE_FILE;
    } else if (S_ISDIR(path_stat.st_mode)) {
        return FILE_TYPE_DIRECTORY;
    } else {
        return FILE_TYPE_OTHER;
    }
}

/* Helper function for deleting directories recursively 
 https://stackoverflow.com/questions/5467725/how-to-delete-a-directory-and-its-contents-in-posix-c
 */
int unlink_cb(const char *fpath, const struct stat *sb, int typeflag, struct FTW *ftwbuf) {
    int rv = remove(fpath);
    if (rv) perror(fpath);
    return rv;
}

// --------------------------------------------------
#pragma mark Ncurses Abstractions
// --------------------------------------------------

/* Small helper function to display a dialog and let the user
 * respond with Yes or No */
bool gitsi_dialog(gitsi_context *context, const char *title) {
    bool verbose = false;
    while (true) {
        standout();
        move(context->max_y - 1, 0);
        clrtoeol();
        mvprintw(context->max_y - 1, 0, "    %s %s [Y]es or [N]o", verbose ? "PLEASE ENTER" : "", title);
        standend();
        int ch = getch();
        enum key_stroke key = translate_key(context, ch);
        if (key == K_YES) {
            return true;
        } else if (key == K_NO) {
            return false;
        } else {
            verbose = true;
        }
    }
    return false;
}

/* clear one line on the screen */
void gitsi_clear_line(gitsi_context *context, size_t row) {
    // This will be reported as a leak as it is never free'ed
    static char *clean_line = NULL;
    // we imagine a max terminal size of 4096
    const size_t line_size = 4096;
    if (clean_line == NULL) {
        clean_line = malloc(line_size * sizeof(char));
    }
    size_t max_clean = MIN((int)line_size, context->max_x);
    memset(clean_line, ' ', sizeof(char) * max_clean);
    clean_line[context->max_x] = '\0';
    mvprintw((int)row, 0, "%s", clean_line);
}

/* Startup ncurses and set the proper flags */
void gitsi_curses_start(gitsi_context *context) {
    initscr();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(0);
    nonl();
    meta(stdscr, true);
    context->has_color = has_colors();
    if (context->has_color) {
        start_color();
        use_default_colors();
        init_pair(GITSI_COLOR_INDEX, COLOR_GREEN, -1);
        init_pair(GITSI_COLOR_UNTRACKED, COLOR_RED, -1);
        init_pair(GITSI_COLOR_TITLE, COLOR_CYAN, -1);
        init_pair(GITSI_COLOR_WORKSPACE, COLOR_YELLOW, -1);
        init_pair(GITSI_COLOR_VISUAL_SELECT, COLOR_BLACK, COLOR_CYAN);
    }
}

/* Stop ncurses and reset the terminal */
void gitsi_curses_stop(bool keepPage) {
    if (!keepPage) {
        clear();
    }
    refresh();
    doupdate();
    endwin();
}

// --------------------------------------------------
#pragma mark Selection & Index Helpers
// --------------------------------------------------

/* Select the first non-category item in the list */
void gitsi_select_first_entry(gitsi_context *context) {
    for (size_t i = 0; i < context->filtered_entry_count; ++i) {
        if (context->filtered_entries[i]->type != STATUS_TYPE_CATEGORY) {
            context->position = context->filtered_entries[i];
            return;
        }
    }
}

/* Select the first item in the given category */
void gitsi_select_category(gitsi_context *context, enum GITSI_STATUS_TYPE type) {
    if (type == STATUS_TYPE_CATEGORY)return;
    for (size_t i = 0; i < context->filtered_entry_count; ++i) {
        if (context->filtered_entries[i]->type == type) {
            context->position = context->filtered_entries[i];
            break;
        }
    }
}

/* Select the last entry in the list */
void gitsi_select_last_entry(gitsi_context *context) {
    context->position = context->filtered_entries[context->filtered_entry_count - 1];
}

/* Get the index of the current context->position entry */
size_t gitsi_position_index(gitsi_context *context) {
    // find the current selection position on screen
    for (size_t cursor_pos = 0; cursor_pos < context->filtered_entry_count; cursor_pos++) {
        if (context->position == context->filtered_entries[cursor_pos])return cursor_pos;
    }
    return 0;
}

/* Select the next entry that is `direction` entries away from the
 * currently selected entry */
void gitsi_select_entry(gitsi_context *context, int direction) {
    int position = 0;
    bool found = false;
    for (size_t i = 0; i < context->filtered_entry_count; ++i) {
        if (context->filtered_entries[i]->type == STATUS_TYPE_CATEGORY)continue;
        if (context->position == context->filtered_entries[i]) {
            position = (int)i;
            found = true;
            break;
        }
    }
    // Due to search, the entry is not in the filtered list anymore
    if (found == false) {
        gitsi_select_first_entry(context);
        return;
    }
    while (true) {
        position += direction;
        if (context->is_visual_mark_mode == true) {
            context->filtered_entries[position]->marked = true;
        }
        if (position < 0) {
            gitsi_select_last_entry(context);
            break;
        }
        if (position >= (int)context->filtered_entry_count) {
            gitsi_select_first_entry(context);
            break;
        }
        if (context->filtered_entries[position]->type == STATUS_TYPE_CATEGORY)continue;
        context->position = context->filtered_entries[position];
        break;
    }
}

/* Select the entry at the position `index` */
void gitsi_select_entry_by_index(gitsi_context *context, size_t index) {
    if (context->filtered_entry_count == 0)return;
    if (index > context->filtered_entry_count) {
        context->position = context->filtered_entries[context->filtered_entry_count - 1];
        return;
    }
    if (context->filtered_entries[index]->type == STATUS_TYPE_CATEGORY) {
        gitsi_select_entry_by_index(context, index + 1);
        return;
    }
    context->position = context->filtered_entries[index];
}

// --------------------------------------------------
#pragma mark Commandline and Git functions
// --------------------------------------------------

/* Print the command line help */
void gitsi_print_help() {
    printf("usage:\t\tgitsi [repository]\n");
    printf("\t\tgitsi without parameters uses the current repository\n");
    exit(0);
}

/* Parse the command line parameters */
void gitsi_parse_parameters(gitsi_context *context, int argc, char *argv[]) {
    const char *repo_dir = ".";
    
    if (argc == 2) {
        if (strcmp(argv[1], "-h") == 0) {
            gitsi_print_help();
        }
        repo_dir = argv[1];
    }
    context->repo_dir = strdup(repo_dir);
}

/* Check if there was an libgit error, if there was, display it and exit */
void gitsi_check_error(const char *source, int error) {
    if (!error)return;
    const git_error *err = giterr_last();
    if (err != NULL) {
        fprintf(stderr, "Source: %s\n", source);
        fprintf(stderr, "Error: %s\n", err->message);
    }
    exit(1);
}

/* Open the git repository */
void gitsi_open_repository(gitsi_context *context) {
    int error = git_repository_open_ext(&context->repo, context->repo_dir, 0, NULL);
    gitsi_check_error("open repository", error);
    
    if (git_repository_is_bare(context->repo)) {
        fprintf(stderr, "Could not report status on bare repository: %s", context->repo_dir);
        exit(1);
    }
    
    // The `repo_dir` is now the common repo dir that git detected
    free(context->repo_dir);
    context->repo_dir = NULL;
    context->repo_dir = git_repository_workdir(context->repo);
}

/* Go through all the entries in the context and free them */
void gitsi_free_entries(gitsi_context *context) {
    // As the `position` points to one of our entries, it also needs to be cleared
    context->position = NULL;
    for (size_t i = 0; i < context->entry_count; ++i) {
        // Not all categories are always there, but we always alloc the space for 3 categories
        // So the last [1-3] entries might never have been allocated
        if (context->entries[i] == NULL)continue;
        free((char*)context->entries[i]->filename);
        free((char*)context->entries[i]->description);
        free(context->entries[i]);
        // Here, we explicitly don't set context->entries[i] to NULL as that would also set
        // context->entries[0] to NULL, and then the next `if` context->entries != NULL would
        // result in false, as context->entries == context->entries[0].
    }
    if (context->entries != NULL) {
        free(context->entries);
        context->entries = NULL;
        context->entry_count = 0;
    }
    
    if (context->filtered_entries != NULL) {
        free(context->filtered_entries);
        context->filtered_entries = NULL;
        context->filtered_entry_count = 0;
    }
}

/* free all the git structures as well as the entries */
void gitsi_cleanup(gitsi_context *context) {
    git_repository_free(context->repo);
    git_index_free(context->repo_index);
    context->repo_index = NULL;
    context->repo = NULL;
    gitsi_free_entries(context);
    git_libgit2_shutdown();
}

/* Add an entry to our list of entries, called when git status is performed */
void gitsi_add_entry(const char *filename, const char *description,
                     enum GITSI_STATUS_TYPE type, gitsi_context *context, size_t pos,
                     git_status_t git_status) {
    context->entries[pos] = calloc(1, sizeof(gitsi_status_entry));
    if (filename != NULL)
        context->entries[pos]->filename = strdup(filename);
    else
        context->entries[pos]->filename = NULL;
    if (description != NULL)
        context->entries[pos]->description = strdup(description);
    else
        context->entries[pos]->description = NULL;
    context->entries[pos]->type = type;
    context->entries[pos]->git_status = git_status;
}

/* Use libgit to get the repository status */
void gitsi_get_repository_status(gitsi_context *context) {
    if(context->entries != NULL) {
        gitsi_free_entries(context);
    }
    if (context->repo_index != NULL) {
        git_index_free(context->repo_index);
        context->repo_index = NULL;
    }
    int error = git_repository_index(&context->repo_index, context->repo);
    gitsi_check_error("git repository index", error);
    
    git_status_options statusopt = GIT_STATUS_OPTIONS_INIT;
    statusopt.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
    statusopt.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED |
    GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX |
    GIT_STATUS_OPT_SORT_CASE_SENSITIVELY;
    git_status_list *status = NULL;
    error = git_status_list_new(&status, context->repo, &statusopt);
    gitsi_check_error("git status list", error);
    
    size_t i, entry_count = 0, maxi = git_status_list_entrycount(status);
    const git_status_entry *s;
    const char *old_path, *new_path, *actual_path;
    bool category = false;
    
    size_t number_of_categories = 3;
    // We account for off-by-one for the number of categories (+ 1)
    // And for the number of entries (+ 1)
    // So we reserver the number of categories + the number of entries + 2 off-by-ones
    size_t number_of_entries = 2 + maxi + number_of_categories;
    context->entries = calloc(number_of_entries, sizeof(gitsi_status_entry*));
    
    // Index
    for (i = 0; i < maxi; ++i) {
        const char *istatus = NULL;
        
        s = git_status_byindex(status, i);
        
        if (s->status == GIT_STATUS_CURRENT)
            continue;
        
        if (s->status & GIT_STATUS_INDEX_NEW)
            istatus = "new file";
        if (s->status & GIT_STATUS_INDEX_MODIFIED)
            istatus = "modified";
        if (s->status & GIT_STATUS_INDEX_DELETED)
            istatus = "deleted";
        if (s->status & GIT_STATUS_INDEX_RENAMED)
            istatus = "renamed";
        if (s->status & GIT_STATUS_INDEX_TYPECHANGE)
            istatus = "typechange";
        
        if (istatus == NULL)
            continue;
        
        if (!category) {
            category = true;
            gitsi_add_entry("Index", NULL, STATUS_TYPE_CATEGORY, context, entry_count, GIT_STATUS_IGNORED);
            entry_count += 1;
        }
        
        old_path = s->head_to_index->old_file.path;
        new_path = s->head_to_index->new_file.path;
        if (old_path && new_path && strcmp(old_path, new_path)) {
            actual_path = old_path;
        } else {
            actual_path = old_path ? old_path : new_path;
        }
        gitsi_add_entry(actual_path, istatus, STATUS_TYPE_INDEX, context, entry_count, s->status);
        entry_count += 1;
    }
    
    category = false;
    
    // Workspace
    for (i = 0; i < maxi; ++i) {
        const char *wstatus = NULL;
        
        s = git_status_byindex(status, i);
        
        if (s->status == GIT_STATUS_CURRENT || s->index_to_workdir == NULL)
            continue;
        
        if (s->status & GIT_STATUS_WT_MODIFIED)
            wstatus = "modified";
        if (s->status & GIT_STATUS_WT_DELETED)
            wstatus = "deleted";
        if (s->status & GIT_STATUS_WT_RENAMED)
            wstatus = "renamed";
        if (s->status & GIT_STATUS_WT_TYPECHANGE)
            wstatus = "typechange";
        
        if (wstatus == NULL)
            continue;
        
        if (!category) {
            category = true;
            gitsi_add_entry("Workspace", NULL, STATUS_TYPE_CATEGORY, context, entry_count, GIT_STATUS_IGNORED);
            entry_count += 1;
        }
        
        old_path = s->index_to_workdir->old_file.path;
        new_path = s->index_to_workdir->new_file.path;
        if (old_path && new_path && strcmp(old_path, new_path)) {
            actual_path = old_path;
        } else {
            actual_path = old_path ? old_path : new_path;
        }
        gitsi_add_entry(actual_path, wstatus, STATUS_TYPE_WORKSPACE, context, entry_count, s->status);
        entry_count += 1;
    }
    
    category = false;
    
    // Untracked
    for (i = 0; i < maxi; ++i) {
        s = git_status_byindex(status, i);
        if (s->status == GIT_STATUS_WT_NEW) {
            if (!category) {
                category = true;
                gitsi_add_entry("Untracked", NULL, STATUS_TYPE_CATEGORY, context, entry_count, GIT_STATUS_IGNORED);
                entry_count += 1;
            }
            gitsi_add_entry(s->index_to_workdir->old_file.path, "untracked", STATUS_TYPE_UNTRACKED, context, entry_count, s->status);
            entry_count += 1;
        }
    }
    
    context->entry_count = entry_count;
    git_status_list_free(status);
}

/* Go through all entries and filter them by filename. The results are stored
 * in `context->filtered_entries` */
void gitsi_filter_entries(gitsi_context *context) {
    if (context->filtered_entries != NULL) {
        free(context->filtered_entries);
        context->filtered_entries = NULL;
    }
    // We're lazy. Instead of having to use realloc, we just reserve as much space for filtered
    // items as for normal items
    context->filtered_entries = calloc(context->entry_count, sizeof(gitsi_status_entry*));
    context->filtered_entry_count = 0;
    for (size_t i = 0; i < context->entry_count; ++i) {
        // Empty search terms match all
        bool has_search_term = strlen(context->search_term) == 0;
        // Headlines always match
        bool is_headline = context->entries[i]->type == STATUS_TYPE_CATEGORY;
        // the actual match
        bool match_term = strstr(context->entries[i]->filename, context->search_term) != NULL;
        if (has_search_term || match_term || is_headline) {
            context->filtered_entries[context->filtered_entry_count] = context->entries[i];
            context->filtered_entry_count += 1;
        }
    }
}

/* Perform the git status and filter it */
void gitsi_update_status(gitsi_context *context) {
    gitsi_get_repository_status(context);
    if (context->entry_count == 0) {
        gitsi_curses_stop(false);
        printf("No entries found\n");
        exit(0);
    }
    gitsi_filter_entries(context);
}

// We need forward declarations here as the functions call each other
void gitsi_checkout_entry(gitsi_context *context, gitsi_status_entry *entry);

/* Stage or add an entry depending on the type of the file / entry */
void gitsi_stage_entry(gitsi_context *context, gitsi_status_entry *entry) {
    if (entry->type == STATUS_TYPE_CATEGORY)return;

    // if the entry is a deleted entry, what we really want to call
    // is git_index_remove_bypath
    if (entry->git_status == GIT_STATUS_WT_DELETED && entry->type == STATUS_TYPE_WORKSPACE) {
        int error = git_index_remove_bypath(context->repo_index, entry->filename);
        gitsi_check_error("git index remove bypath", error);
    }

    int error;
    switch (util_is_regular_file(context->repo_dir, entry->filename)) {
        case FILE_TYPE_FILE:
            error = git_index_add_bypath(context->repo_index, entry->filename);
            gitsi_check_error("git index write", error);
            break;
        case FILE_TYPE_DIRECTORY: {
            git_strarray arr = { .strings = (char**)&entry->filename, .count = 1};
            error = git_index_add_all(context->repo_index, &arr, 0, NULL, NULL);
            gitsi_check_error("git index write", error);
            break;
        }
        case FILE_TYPE_OTHER:
            return;
    }
    error = git_index_write(context->repo_index);
    gitsi_check_error("git index write", error);
}

/* Unstage an entry that is in the workspace */
void gitsi_unstage_workspace(gitsi_context *context, gitsi_status_entry *entry) {

    // if the entry is a deleted entry, what we really want to call
    // is reset as we want to eradicate the deletion
    if (entry->git_status == GIT_STATUS_WT_DELETED && entry->type == STATUS_TYPE_WORKSPACE) {
        gitsi_checkout_entry(context, entry);
        return;
    }

    // Unstage in the workspace means delete
    int error = git_index_remove_bypath(context->repo_index, entry->filename);
    gitsi_check_error("git index remove bypath", error);
}

/* Unstage an entry that is on the index */
void gitsi_unstage_index(gitsi_context *context, gitsi_status_entry *entry) {
    // Unstage in the index means workspace. This is kinda complicated.
    char *paths[] = { (char*)entry->filename, };
    
    git_strarray pathspecs = { .strings = paths, .count = 1 };
    git_reference *head;
    git_object *head_commit;
    
    git_repository_head(&head, context->repo);
    git_reference_peel(&head_commit, head, GIT_OBJ_COMMIT);
    git_reset_default(context->repo, head_commit, &pathspecs);
}

/* Unstage an entry that is untracked. I.e. delete it */
void gitsi_unstage_untracked(gitsi_context *context, gitsi_status_entry *entry) {
    char *message;
    asprintf(&message, "Delete File '%s'?", entry->filename);
    bool result = gitsi_dialog(context, (const char*)message);
    free(message);
    char *buffer;
    asprintf(&buffer, "%s/%s", context->repo_dir, entry->filename);
    if (result) {
        switch (util_is_regular_file(context->repo_dir, entry->filename)) {
            case FILE_TYPE_FILE:
                remove(buffer);
                break;
            case FILE_TYPE_DIRECTORY: {
                nftw(buffer, unlink_cb, 64, FTW_DEPTH | FTW_PHYS);
                break;
            }
            case FILE_TYPE_OTHER:
                return;
        }
        // build the full path
    }
    free(buffer);
}

/* Unstage or delete an entry, depending on the type of a file */
void gitsi_unstage_entry(gitsi_context *context, gitsi_status_entry *entry) {
    if (entry->type == STATUS_TYPE_CATEGORY)return;
    switch (entry->type) {
        case STATUS_TYPE_WORKSPACE:
            gitsi_unstage_workspace(context, entry);
            break;
        case STATUS_TYPE_INDEX:
            gitsi_unstage_index(context, entry);
            break;
        case STATUS_TYPE_UNTRACKED:
            gitsi_unstage_untracked(context, entry);
            break;
        case STATUS_TYPE_CATEGORY:
            break;
    }
    // Write out
    int error = git_index_write(context->repo_index);
    gitsi_check_error("git index write", error);
}

/* Checkout an entry, i.e. remove all changes */
void gitsi_checkout_entry(gitsi_context *context, gitsi_status_entry *entry) {
    git_checkout_options opts;
    git_checkout_init_options(&opts, GIT_CHECKOUT_OPTIONS_VERSION);
    opts.checkout_strategy = GIT_CHECKOUT_FORCE;

    char *paths[] = { (char*)entry->filename, };
    opts.paths.strings = paths;
    opts.paths.count = 1;

    int error = git_checkout_head(context->repo, &opts);
}

/* Perform action `action` on all marked entries */
void gitsi_action_on_marked(gitsi_context *context, 
                            void action (gitsi_context *context, gitsi_status_entry *entry)) {
    size_t cursor_pos = gitsi_position_index(context);
    
    // After the action, many marked items, including probably cursor pos
    // may have moved. We need a new position.
    // We use the first non-marked item (including position) after position
    bool found = false;
    for (size_t i = cursor_pos; i < context->filtered_entry_count; ++i) {
        if (context->filtered_entries[i]->type == STATUS_TYPE_CATEGORY)continue;
        if (context->filtered_entries[i]->marked == true)continue;
        cursor_pos = i;
        found = true;
        break;
    }
    
    for (size_t i = 0; i < context->entry_count; i++) {
        gitsi_status_entry *entry = context->entries[i];
        if (entry->marked == true) {
            action(context, entry);
            entry->marked = false;
        }
    }
    // Set the new position
    if (found == false) {
        gitsi_select_first_entry(context);
    } else {
        context->position = context->filtered_entries[cursor_pos];
    }
}

/* perform git diff and display it in a pager */
void gitsi_perform_diff(gitsi_context *context, gitsi_status_entry *entry) {
    const char param_index[] = "--cached";
    const char param_workspace[] = "";
    const char param_untracked[] = "--no-index /dev/null";
    const char *param;
    
    switch (entry->type) {
        case STATUS_TYPE_INDEX: param = param_index; break;
        case STATUS_TYPE_WORKSPACE: param = param_workspace; break;
        case STATUS_TYPE_UNTRACKED: param = param_untracked; break;
        case STATUS_TYPE_CATEGORY: return;
    }
    
    char *buffer;
    asprintf(&buffer, "/bin/sh -c \"cd '%s' && git diff %s '%s'\"", context->repo_dir, param, entry->filename);
    
    gitsi_curses_stop(false);
    char *oldenv_org = getenv("GIT_PAGER");
    char *oldenv = (oldenv_org == NULL) ? NULL : strdup(getenv("GIT_PAGER"));
    setenv("GIT_PAGER", "less -RSX -+F", 1);
    system("clear");
    system(buffer);
    unsetenv("GIT_PAGER");
    if (oldenv != NULL) {
        setenv("GIT_PAGER", oldenv, 1);
        free(oldenv);
    }
    free(buffer);
    gitsi_curses_start(context);
}

/* perform git add -p */
void gitsi_perform_gitp(gitsi_context *context, gitsi_status_entry *entry) {
    char *buffer;
    asprintf(&buffer, "/bin/sh -c \"cd '%s' && git add -p '%s'\"",
             context->repo_dir,
             entry->filename);
    
    gitsi_curses_stop(false);
    system("clear");
    system(buffer);
    gitsi_curses_start(context);
    free(buffer);
}

/* perform a git commit with the external $EDITOR. Cana lso be an `amend` commit */
void gitsi_perform_commit(gitsi_context *context, bool amend) {
    char *buffer;
    asprintf(&buffer, "/bin/sh -c \"cd '%s' && git commit %s\"", context->repo_dir,
             amend == true ? "--amend" : "");
    
    gitsi_curses_stop(false);
    system("clear");
    system(buffer);
    gitsi_curses_start(context);
    free(buffer);
}

/* perform a git push */
void gitsi_perform_push(gitsi_context *context) {
    char *buffer;
    asprintf(&buffer, "/bin/sh -c \"cd '%s' && git push\"", context->repo_dir);
    
    gitsi_curses_stop(false);
    system("clear");
    system(buffer);
    gitsi_curses_start(context);
    free(buffer);
}

/* perform a git push -u */
void gitsi_perform_pushu(gitsi_context *context) {
    char *buffer;
    asprintf(&buffer, "/bin/sh -c \"cd '%s' && git push -u\"", context->repo_dir);
    
    gitsi_curses_stop(false);
    system("clear");
    system(buffer);
    gitsi_curses_start(context);
    free(buffer);
}

void gitsi_perform_edit(gitsi_context *context, gitsi_status_entry *entry) {
    char *buffer;
    asprintf(&buffer, "/bin/sh -c \"cd '%s' && vi '%s'\"", context->repo_dir, entry->filename);
    
    gitsi_curses_stop(false);
    system("clear");
    system(buffer);
    gitsi_curses_start(context);
    free(buffer);
}

void gitsi_perform_command(gitsi_context *context, const char *command) {
    char *buffer;
    asprintf(&buffer, "/bin/sh -c \"cd '%s'; git %s\"", context->repo_dir, command);

    gitsi_curses_stop(false);
    system("clear");
    system(buffer);
    gitsi_curses_start(context);
    free(buffer);
}

// --------------------------------------------------
#pragma mark Printing / UI
// --------------------------------------------------

/* Dynamically return the action for the S and U key (depending on the entry type) */
void gitsi_action_names(gitsi_context *context, const char** first, const char** second) {
    if (context->position == NULL) return;
    switch (context->position->type) {
        case STATUS_TYPE_INDEX:
            *first = "";
            *second = "unstage";
            break;
        case STATUS_TYPE_WORKSPACE:
            *first = "stage";
            *second = "stage delete";
            break;
        case STATUS_TYPE_UNTRACKED:
            *first = "stage";
            *second = "delete file";
            break;
        case STATUS_TYPE_CATEGORY:
            return;
    }
}

/* print the search bar at the bottom */
void gitsi_print_status_search(gitsi_context *context, size_t row) {
    const char title[] = "/";
    mvprintw((int)row, 1, "%s%s", title, context->search_term);
    const char search_help[] = "[Enter: back to list] [Escape: Cancel]";
    const char search_help_short[] = "[ENTER|ESC]";
    const int length_help = strlen(search_help);
    const int length_help_short = strlen(search_help_short);
    if ((context->max_x - (strlen(context->search_term) + 4)) > length_help) {
        mvprintw((int)row, context->max_x - (length_help + 1), search_help);
    } else {
        mvprintw((int)row, context->max_x - (length_help_short + 1), search_help_short);
    }
}

/* Print the command bar at the bottom */
void gitsi_print_command(gitsi_context *context, size_t row) {
    const char title[] = ":";
    mvprintw((int)row, 1, "%s%s", title, context->command_term);
    const char search_command[] = "[Enter: run GIT command] [Escape: Cancel]";
    const char search_command_short[] = "[ENTER|ESC]";
    const int length_command = strlen(search_command);
    const int length_command_short = strlen(search_command_short);
    if ((context->max_x - (strlen(context->command_term) + 4)) > length_command) {
        mvprintw((int)row, context->max_x - (length_command + 1), search_command);
    } else {
        mvprintw((int)row, context->max_x - (length_command_short + 1), search_command_short);
    }
}

/* Dynamically print the bottom status help. It will print as many help
 * entries as possible with the given width */
void gitsi_print_status_help(gitsi_context *context, size_t row) {
    const char *help_help = "[h: HELP]";
    const char *action_add_name = "";
    const char *action_del_name = "";
    gitsi_action_names(context, &action_add_name, &action_del_name);
    
    size_t help_position = context->max_x - (1 + strlen(help_help));
    int remaining_space = (int)help_position;
    size_t current_x_position = 1;
    for (size_t i = 0; i < help_entries_length; i++) {
        char *title;
        
        const char *name = help_entries[i].name;
        if (strcmp(name, "ACTION_A") == 0) {
            name = action_add_name;
        }
        else if (strcmp(name, "ACTION_B") == 0) {
            name = action_del_name;
        }
        if (strlen(name) == 0)continue;
        asprintf(&title, "[%s: %s] ", help_entries[i].key, name);
        remaining_space -= strlen(title);
        if (remaining_space < 0) {
            free(title);
            break;
        }
        mvprintw((int)row, (int)current_x_position, title);
        current_x_position += strlen(title);
        free(title);
    }
    mvprintw((int)row, 1 + (int)help_position, help_help);
}

/* Print the status bar at the bottom. Either help or search */
void gitsi_print_statusbar(gitsi_context *context) {
    attrset(A_BOLD | A_STANDOUT);
    gitsi_clear_line(context, context->max_y - 1);
    if (context->is_search || strlen(context->search_term) > 0) {
        gitsi_print_status_search(context, context->max_y - 1);
    } else if (context->is_in_command_mode || strlen(context->command_term) > 0) {
        gitsi_print_command(context, context->max_y - 1);
    } else {
        gitsi_print_status_help(context, context->max_y - 1);
    }
    attrset(0);
}

/* Print / scroll the current list of entries */
void gitsi_print_list(gitsi_context *context) {
    const size_t status_bar_height = 2;
    const size_t lpos = 6;
    size_t count = context->filtered_entry_count;
    gitsi_status_entry **entries = context->filtered_entries;
    
    size_t cursor_pos = gitsi_position_index(context);
    
    // determine the beginning and end of the displayed page
    // based on cursor_position and height of screen
    size_t list_height = context->max_y - status_bar_height;
    // Have to be signed so we can see if we're <0
    int lowerlimit = (int)(cursor_pos - list_height / 2);
    int upperlimit = (int)(count - (list_height / 2));
    size_t start_pos = MAX(MIN(lowerlimit, upperlimit), 0);
    size_t length = list_height;
    
    // if all items fit on the list, ignore scrolling
    if (count < list_height)start_pos = 0;
    
    // line number calculation. We have to iterate up to the middle, at least once
    int middle = 0;
    for (size_t i = start_pos; i < count; ++i) {
        if (entries[i]->type == STATUS_TYPE_CATEGORY)continue;
        if (i > (start_pos + length))break;
        middle += 1;
        if (context->position == entries[i]) {
            break;
        }
    }
    
    // We also have to calculate the longest title and the longest description
    // so that we can align the fields
    size_t longest_title = 0;
    size_t longest_description = 0;
    for (size_t i = start_pos; i < count; ++i) {
        if (entries[i]->type == STATUS_TYPE_CATEGORY) { continue; }
        if (entries[i]->filename != NULL) {
            size_t l = strlen(entries[i]->filename);
            if (l > longest_title) {
                longest_title = l;
            }
        }
        if (entries[i]->description != NULL) {
            size_t l = strlen(entries[i]->description);
            if (l > longest_description) {
                longest_description = l;
            }
        }
    }
    
    
    int linum_pos = 1;
    for (size_t i = start_pos; i < count; ++i) {
        if (i > (start_pos + length))break;
        int pos = ((int)i) - (int)start_pos;
        bool is_selected = context->position == entries[i];
        bool is_marked = entries[i]->marked;
        
        if (context->has_color == true && !is_selected) {
            if (entries[i]->type == STATUS_TYPE_INDEX) {
                color_set(GITSI_COLOR_INDEX, 0);
            } else if (entries[i]->type == STATUS_TYPE_CATEGORY) {
                color_set(GITSI_COLOR_TITLE, 0);
            } else if (entries[i]->type == STATUS_TYPE_WORKSPACE) {
                color_set(GITSI_COLOR_WORKSPACE, 0);
            } else if (entries[i]->type == STATUS_TYPE_UNTRACKED) {
                color_set(GITSI_COLOR_UNTRACKED, 0);
            }
        }
        
        if (context->is_visual_mark_mode == true && (is_marked || is_selected)) {
            color_set(GITSI_COLOR_VISUAL_SELECT, 0);
        }
        if (is_selected)attron(A_STANDOUT);
        
        gitsi_clear_line(context, pos);
        if (entries[i]->type == STATUS_TYPE_CATEGORY) {
            mvprintw(pos, lpos, entries[i]->filename);
            color_set(GITSI_COLOR_VISUAL_SELECT, 0);
            mvprintw(pos, 0, "    ");
        } else {
            const char* filename = entries[i]->filename;
            if (filename == NULL)filename = "";
            const char* description = entries[i]->description;
            if (description == NULL)description = "";
            size_t c = lpos;
            const char *marker = is_marked ? "*" : " ";
            mvprintw(pos, c, marker);
            c += strlen(marker);
            c += 1;
            mvprintw(pos, c, filename);
            c += longest_title + 1;
            mvprintw(pos, c, description);
            
            color_set(GITSI_COLOR_VISUAL_SELECT, 0);
            mvprintw(pos, 0, "%3d ", abs(middle - linum_pos));
            linum_pos += 1;
        }
        
        attrset(0);
    }
    
    // clear the remaining lines
    for (size_t i = (count - start_pos); i < (size_t)context->max_y; i++) {
        gitsi_clear_line(context, i);
    }
}

/* Print the full help screen (i.e. `h` key) */
void gitsi_print_full_help(gitsi_context *context) {
    clear();
    color_set(GITSI_COLOR_TITLE, 0);
    attrset(A_BOLD);
    mvprintw(1, 2, "Help [Press any key to go back]");
    attrset(0);
    color_set(0, 0);
    size_t i = 3;
    for (; i < help_entries_length; i++) {
        mvprintw((int)i, 2, "[%s]\t%s", help_entries[i].key, help_entries[i].desc);
    }
    i += 2;
    mvprintw((int)i, 2, "Use 1-9 before j/k/C-d/C-u to repeat the action [like vi]");
}

// --------------------------------------------------
#pragma mark Main Logic
// --------------------------------------------------

/* The main print function thta decided what to do based on `context` */
void gitsi_print_main(gitsi_context *context) {
    if (context->is_in_help == true) {
        gitsi_print_full_help(context);
    } else {
        gitsi_print_list(context);
        gitsi_print_statusbar(context);
    }
}

/* Logic to handle searching */
void gitsi_process_search(gitsi_context *context, enum key_stroke key, int ch) {
    if (strlen(context->search_term) > MAX_INPUT_CHARS)return;
    if (key == K_ENTER) {
        context->is_search = false;
        // if the position is not part of the search anymore, change position to first
        bool found = false;
        if (context->position != NULL) {
            for (size_t i = 0; i < context->filtered_entry_count; i++) {
                if (context->filtered_entries[i] == context->position) {
                    found = true;
                    break;
                }
            }
            if (found == false) {
                gitsi_select_first_entry(context);
            }
        }
        return;
    }
    if (key == K_ESC) {
        context->is_search = false;
        strcpy(context->search_term, "");
    }
    else if (key == K_BACKSPACE) {
        int new_pos = ((int)strlen(context->search_term)) - 1;
        if (new_pos >= 0) {
            context->search_term[new_pos] = '\0';
        }
    }
    else {
        sprintf(context->search_term, "%s%c", context->search_term, ch);
    }
    gitsi_filter_entries(context);
}

/* Logic to enter commands */
void gitsi_process_command_input(gitsi_context *context, enum key_stroke key, int ch) {
    if (strlen(context->command_term) > MAX_INPUT_CHARS)return;
    if (key == K_ENTER) {
        context->is_in_command_mode = false;
        if (strlen(context->command_term) > 0) {
            gitsi_perform_command(context, context->command_term);
        }
        strcpy(context->command_term, "");
        return;
    }
    if (key == K_ESC) {
        context->is_in_command_mode = false;
        strcpy(context->command_term, "");
    }
    else if (key == K_BACKSPACE) {
        int new_pos = ((int)strlen(context->command_term)) - 1;
        if (new_pos >= 0) {
            context->command_term[new_pos] = '\0';
        }
    }
    else {
        sprintf(context->command_term, "%s%c", context->command_term, ch);
    }
}

/* Signal Handler for SIGINT 
 * If the user hits CTRL+C to exit, we still want to clean up properly
 */
volatile sig_atomic_t sigint_received = false;
void sigint_handler(int sig_num) { 
    /* Reset handler to catch SIGINT next time.
     Refer http://en.cppreference.com/w/c/program/signal */
    signal(SIGINT, sigint_handler);
    sigint_received = true;
} 

void gitsi_process_input(gitsi_context *context, int input_char) {
    enum key_stroke key = translate_key(context, input_char);
    
    if (context->is_search) {
        gitsi_process_search(context, key, input_char);
    } else if (context->is_in_command_mode) {
        gitsi_process_command_input(context, key, input_char);
    } else if (context->is_in_help) {
        context->is_in_help = false;
    } else {
        int iteration_count = 1;
        if (context->number_stack_count > 0) {
            iteration_count = atoi(context->number_stack);
        }
        // detect 0-9
        if (key == K_OTHER && input_char >= 48 && input_char <= 57) {
            if (context->number_stack_count >= (MAX_NUMBER_STACK - 1))return;
            context->number_stack[context->number_stack_count++] = (char)input_char;
        } else if (key == K_SLASH) {
            context->is_search = true;
        } else if (key == K_ESC) {
            if (strlen(context->search_term) > 0) {
                strcpy(context->search_term, "");
                gitsi_filter_entries(context);
            }
            else if (context->is_visual_mark_mode == true) {
                context->is_visual_mark_mode = false;
                for (size_t i = 0; i < context->entry_count; i++) {
                    context->entries[i]->marked = false;
                }
            }
        } else if (key == K_Q) {
            sigint_received = true;
            return;
        } else if (key == K_H || key == K_HELP) {
            context->is_in_help = true;
        }
        else if (key == K_J || key == K_ARROW_DOWN) {
            for (int i = 0; i < iteration_count; i++) {
                gitsi_select_entry(context, 1);
            }
        }
        else if (key == K_K || key == K_ARROW_UP) {
            for (int i = 0; i < iteration_count; i++) {
                gitsi_select_entry(context, -1);
            }
        }
        else if (key == K_C_D) {
            for (int i = 0; i < iteration_count; i++) {
                gitsi_select_entry(context, 10);
            }
        }
        else if (key == K_C_U) {
            for (int i = 0; i < iteration_count; i++) {
                gitsi_select_entry(context, -10);
            }
        }
        else if (key == K_S_G) {
            gitsi_select_last_entry(context);
        }
        else if (key == K_G) {
            gitsi_select_first_entry(context);
        }
        else if (key == K_S) {
            size_t pos = gitsi_position_index(context);
            gitsi_stage_entry(context, context->position);
            gitsi_update_status(context);
            gitsi_select_entry_by_index(context, pos);
        }
        else if (key == K_U) {
            size_t pos = gitsi_position_index(context);
            gitsi_unstage_entry(context, context->position);
            gitsi_update_status(context);
            gitsi_select_entry_by_index(context, pos);
        }
        else if (key == K_S_S) {
            gitsi_action_on_marked(context, &gitsi_stage_entry);
            gitsi_update_status(context);
        }
        else if (key == K_S_U) {
            gitsi_action_on_marked(context, &gitsi_unstage_entry);
            gitsi_update_status(context);
        }
        else if (key == K_I) {
            if (context->position != NULL) {
                gitsi_perform_gitp(context, context->position);
                gitsi_update_status(context);
            }
        }
	else if (key == K_R) {
            gitsi_update_status(context);
	}
        else if (key == K_C) {
            gitsi_perform_commit(context, false);
            gitsi_update_status(context);
        }
        else if (key == K_S_C) {
            gitsi_perform_commit(context, true);
            gitsi_update_status(context);
        }
	else if (key == K_P) {
            gitsi_perform_push(context);
            gitsi_update_status(context);
	}
	else if (key == K_S_P) {
            gitsi_perform_pushu(context);
            gitsi_update_status(context);
	}
        else if (key == K_X) {
            if (context->position == NULL)return;
            if (context->position->type == STATUS_TYPE_UNTRACKED)return;
            bool shouldCheckout = gitsi_dialog(context, "Do you really want to reset all changes to this file?");
            if (shouldCheckout == true) {
                size_t pos = gitsi_position_index(context);
                gitsi_checkout_entry(context, context->position);
                context->position = NULL;
                gitsi_update_status(context);
                gitsi_select_entry_by_index(context, pos);
            }
        }
        else if (key == K_D) {
            if (context->position != NULL) {
                gitsi_perform_diff(context, context->position);
            }
        }
        else if (key == K_E) {
            if (context->position != NULL) {
                gitsi_perform_edit(context, context->position);
		gitsi_update_status(context);
            }
        }
        else if (key == K_COMMAND) {
            context->is_in_command_mode = true;
        }
        else if (key == K_S_1) {
            gitsi_select_category(context, STATUS_TYPE_INDEX);
        }
        else if (key == K_S_2) {
            gitsi_select_category(context, STATUS_TYPE_WORKSPACE);
        }
        else if (key == K_S_3) {
            gitsi_select_category(context, STATUS_TYPE_UNTRACKED);
        }
        else if (key == K_M) {
            if (context->position != NULL) {
                context->position->marked = !context->position->marked;
            }
        }
        else if (key == K_S_V) {
            // Only if visual mark mode was off, do we modify the current position
            if (!context->is_visual_mark_mode && context->position != NULL) {
                context->position->marked = !context->position->marked;
            }
            context->is_visual_mark_mode = !context->is_visual_mark_mode;
        }
        else if (key == K_S_M) {
            // get the current section from position, and iterate over all in section
            if (context->position != NULL && context->position->type != STATUS_TYPE_CATEGORY) {
                enum GITSI_STATUS_TYPE type = context->position->type;
                bool flag = !context->position->marked;
                for (size_t i = 0; i < context->entry_count; i++) {
                    if (context->entries[i]->type == type) {
                        context->entries[i]->marked = flag;
                    }
                }
            }
        }
    }
    
    // Clear number stack on any other action
    if (context->number_stack_count > 0 && key != K_OTHER) {
        context->number_stack_count = 0;
        // Reset the memory
        for (int i = 0; i < MAX_NUMBER_STACK; i++) {
            context->number_stack[i] = 0;
        }
    }
}


/* Main function to process the user input and act
 * accordingly */
void gitsi_main_loop(gitsi_context *context) {
    int ch = 0;
    
    while(true) {
        getmaxyx(stdscr, context->max_y, context->max_x);
        gitsi_print_main(context);
        
        if (context->number_stack_count > 0) {
            context->number_stack[context->number_stack_count + 1] = '\0';
            mvaddstr(0, context->max_x - context->number_stack_count, context->number_stack);
        }
        
        while (true) {
            timeout(100);
            ch = getch();
            // The user pressed CTRL-C, we want to clean up
            if (sigint_received == true) {
                return;
            }
            if (ch != ERR)break;
        }
        
        gitsi_process_input(context, ch);
    }
}

int main(int argc, char *argv[]) {
    for (int argi = 1; argi < argc; argi++) {
        if (strcmp(argv[argi], "--debug-terminal") == 0)
        {
            printf("Debugging in terminal enabled\n");
            getchar(); // Without this call debugging will be skipped
            break;
        }
    }
    
    signal(SIGINT, sigint_handler);
    
    gitsi_context context = {
        .repo = NULL,
        .has_color = false,
        .position = NULL,
        .search_term = "",
        .filtered_entries = NULL,
        .is_search = false,
        .is_in_help = false,
        .is_visual_mark_mode = false,
        .number_stack_count = 0,
    };
#if DEBUG
    context.logfile = fopen(LOGFILE_NAME, "w");
#endif
    git_libgit2_init();
    gitsi_parse_parameters(&context, argc, argv);
    gitsi_open_repository(&context);
    gitsi_curses_start(&context);
    gitsi_update_status(&context);
    gitsi_select_first_entry(&context);
    gitsi_main_loop(&context);
    gitsi_curses_stop(false);
    gitsi_cleanup(&context);
#if DEBUG
    fclose(context.logfile);
#endif
    return 0;
}

// Currently unused. This can be used to debug the usage
// without curses. In the future This will be used to
// run tests
int debug_main(int argc, char *argv[]) {
    gitsi_context context = {
        .repo = NULL,
        .has_color = false,
        .position = NULL,
        .search_term = "",
        .filtered_entries = NULL,
        .is_search = false,
        .is_in_help = false,
        .is_visual_mark_mode = false,
        .number_stack_count = 0,
    };
    context.repo_dir = strdup("test_repository");
    git_libgit2_init();
    gitsi_open_repository(&context);
    gitsi_update_status(&context);
    gitsi_select_first_entry(&context);
    // Test going down
    gitsi_process_input(&context, 'j');
    gitsi_process_input(&context, 'j');
    gitsi_process_input(&context, 'j');
    strcpy(context.search_term, "main");
    gitsi_filter_entries(&context);
    
    for (size_t i=0; i<context.filtered_entry_count; i++) {
        printf("%s\n", context.filtered_entries[i]->filename);
    }
    
    gitsi_cleanup(&context);
    return 0;
}
