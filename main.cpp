#include <iostream>
#include <filesystem>
#include <chrono>

#include <unordered_map>
#include <vector>
#include <set>

#include <fstream>
#include <sstream>

#include "rang.hpp" // Taken from https://github.com/agauniyal/rang, thak you "agauniyal"

#define FILE_TRACK_PATH "file_trackings.ftk"
#define CONFIG_FILE_PATH "backup_config.cfg"

namespace fs = std::filesystem;

/**
 * This structure associates a file path to the time of its last backup for the system to know
 * whether the file must be copied again or not. Objects of this class are stored and loaded from 
 * the file_trackings.ftk binary file.
 * */
struct FileBackupStatus
{
    fs::path path;
    fs::file_time_type last_update;

    FileBackupStatus(){}

    FileBackupStatus(fs::path path, fs::file_time_type last_update) : path(path), last_update(last_update){}

    void save(std::ofstream& out)
    {
        if(!out.is_open())
            return;
        
        size_t len = path.string().size();
        out.write(reinterpret_cast<char*>(&len), sizeof(size_t)); // saving the length of the string
        out.write(path.string().c_str(), len); // saving the string

        auto raw_ticks = last_update.time_since_epoch().count();
        out.write(reinterpret_cast<char*>(&raw_ticks), sizeof(raw_ticks)); // saving last access time
    }

    bool load(std::ifstream& in)
    {
        if(!in.is_open())
            return false;

        // Reading the length of the path string to read
        size_t len;
        if(!in.read(reinterpret_cast<char*>(&len), sizeof(size_t))) // Control if the reading was correct
            return false;

        // Reading the string in a temporal c-string (char*)
        char* temp = new char[len+1];
        if(!in.read(temp, len)) // Control if the reading was correct
            return false;

        temp[len] = '\0';
        
        // Assigning the string value
        path = temp;
        delete [] temp;

        // Reading the time as numeric value
        decltype(fs::file_time_type().time_since_epoch().count()) raw_ticks = 0;
        if(!in.read(reinterpret_cast<char*>(&raw_ticks), sizeof(raw_ticks))) // Control if the reading was correct
            return false;

        // Assigning the chrono time value
        last_update = fs::file_time_type(fs::file_time_type::duration(raw_ticks));

        return true;
    }
};

/**
 * This class handles the read stream of the configuration file, it is used by the Backup Manager in its
 * inicialization to know where and how to make the backup backup_config.cfg
 * */
class ConfigFileReader
{
private:
    
    std::ifstream _file_stream;
    std::vector<std::string> _line_words;
    size_t _act_word_indx = 0;

    // Loads a line in the stringstream
    bool load_line()
    {
        std::string line;
        if(std::getline(_file_stream, line)) {

            _line_words = preprocess_line_buffer(line); // Divide line in text chunks to process them
            _act_word_indx = 0;

            return true;
        }
        else {
            return false;
        }
    }

    std::vector<std::string> preprocess_line_buffer(const std::string& line)
    {
        const std::string SPECIAL_CHARS = "{}=,";
        const std::string SPACES = " \t\n\0";

        std::vector<std::string> word_blocks;
        bool reading_string = false;
        std::string current_word = "";

        for(int i = 0; i < line.size(); ++i)
        {
            char current_char = line[i];

            if(reading_string) // While reading a literal string
            {                
                current_word += current_char;

                if(current_char == '\"') {
                    reading_string = false;
                    word_blocks.push_back(current_word);
                    current_word = "";
                }
            }
            else if(current_char == '"')
            {
                reading_string = true;

                if(current_word.size() > 0) {
                    word_blocks.push_back(current_word);
                }

                current_word = "\"";
            }
            else if(SPECIAL_CHARS.find(current_char) != std::string::npos) // If a special character is found
            {
                if(current_word.size() > 0) {
                    word_blocks.push_back(current_word);
                    current_word = "";
                }

                word_blocks.push_back((std::string() + current_char));
            }
            else if(SPACES.find(current_char) == std::string::npos) // If text is found
            { 
                current_word += current_char;
            }
            else // If an space is found
            { 
                if(current_word.size() > 0) {
                    word_blocks.push_back(current_word);
                    current_word = "";
                }
            }
        }

        if(current_word.size() > 0) // The remaining word at the end of the line if there is one
            word_blocks.push_back(current_word);

        return word_blocks;
    }

public:

    bool open()
    {
        _file_stream.open(CONFIG_FILE_PATH, std::ios::in);

        return _file_stream.is_open();
    }

    std::string next_word()
    {
        if (!_file_stream.is_open() || _file_stream.eof() && _act_word_indx == _line_words.size())
            return "\0";

        std::string buffer;

        if(_act_word_indx < _line_words.size()) {
            return _line_words[_act_word_indx++];
        }
        else if(load_line() && !_line_words.empty()) {
            _act_word_indx = 1;
            return _line_words[0];
        }

        // If line is empty
        load_line();
        return next_word();
    }

    bool eof()
    {
        return _file_stream.eof() && _act_word_indx == _line_words.size();
    }

    // Utility for reading an array of the backup_config file after haven read its name
    std::vector<std::string> read_str_array()
    {
        std::vector<std::string> arr;

        std::string buff = next_word(); // Read '='
        if(buff != "=") // Syntax error
            return arr;

        buff = next_word(); // Read '{'
        if(buff != "{") // Syntax error
            return arr;

        buff = next_word(); // Read the first string or '}'

        while(buff != "}") {
            // buff should contain a string value
            buff = buff.substr(1, buff.size() - 2); // Quit '"' simbols from the string
            arr.push_back(buff);
            
            buff = next_word(); // Read ',' or '}'

            if(buff == ",")
                buff = next_word();
        }

        return arr;
    }

    // Utility for reading a variable
    std::string read_variable()
    {
        std::string buff = next_word(); // Read '='
        
        if(buff != "=") // Syntax error
            return "\0";

        return next_word();
    }
};

class BackupManager
{
private:
    std::unordered_map<std::string, FileBackupStatus> tracked_files;

protected:
    // Metadata from the configuration file
    std::set<std::string> ignored_extensions;
    std::set<std::string> ignored_directories;
    std::vector<std::string> backuped_directories;
    std::string backup_dir;
    bool follow_gitignores;

public:

    BackupManager()
    {
        // Default values
        backuped_directories = {"C:\\Users\\telmo\\OneDrive\\Escritorio\\UCM TKB","C:\\Users\\telmo\\Downloads"};
        backup_dir = "D:\\Backup";
        follow_gitignores = true;
        
        // Trying to read from the configuration file
        ConfigFileReader confg_stream;

        if(!confg_stream.open()) {
            std::cout << rang::fg::magenta << "[!] The configuration file '" << CONFIG_FILE_PATH << "' was not found or couldn't be opened.\n";
            std::cout << rang::fg::reset;
            return;
        }

        std::string buffer = confg_stream.next_word();

        while(!confg_stream.eof())
        {
            if(buffer == "backuped_directories") {
                backuped_directories = confg_stream.read_str_array();
            }
            else if(buffer == "ignored_extensions") {
                for(auto s : confg_stream.read_str_array())
                    ignored_extensions.insert(s);
            }
            else if(buffer == "ignored_directories") {
                for(auto s : confg_stream.read_str_array())
                    ignored_directories.insert(s);
            }
            else if(buffer == "backup_directory"){
                backup_dir = confg_stream.read_variable();
                backup_dir = backup_dir.substr(1, backup_dir.size() - 2); // Quiting '"' symbols
            }
            else if(buffer == "follow_gitignores") {
                std::string val = confg_stream.read_variable();
                follow_gitignores = (val == "true");
            }

            buffer = confg_stream.next_word();
        }
    }

    void print_backup_data()
    {        
        std::cout << rang::fg::yellow << "Saving backup at: " << rang::fg::cyan << backup_dir << "\n";

        std::cout << rang::fg::yellow << "Follow .gitignore files: " << rang::fg::magenta << rang::style::bold << (follow_gitignores ? "true" : "false") << "\n";
        std::cout << rang::style::reset;

        std::cout << rang::fg::yellow << "Scanning directories: ";
        for(int i = 0; i < backuped_directories.size(); ++i) {
            std::cout << rang::fg::cyan << backuped_directories[i];
            if(i < backuped_directories.size() - 1)
                std::cout << rang::fg::reset << " | ";
        }
        std::cout << "\n";

        std::cout << rang::fg::yellow << "Ignored file extensions: ";
        int i = 0;
        for(std::string s : ignored_extensions) {
            std::cout << rang::fg::cyan << s;
            if(i < ignored_extensions.size() - 1)
                std::cout << rang::fg::reset << " | ";
            ++i;
        }
        std::cout << "\n";

        std::cout << rang::fg::yellow << "Ignored directories: ";
        i = 0;
        for(std::string s : ignored_directories) {
            std::cout << rang::fg::cyan << s;
            if(i < ignored_directories.size() - 1)
                std::cout << rang::fg::reset << " | ";
            ++i;
        }
        std::cout << rang::fg::reset << "\n";
    }

    void make_backup()
    {
        // Define a threshold of time for the untracked files found
        auto now_tm = fs::file_time_type::clock::now();
        auto tree_weeks_ago_tm = now_tm - std::chrono::hours(24 * 21);

        setup_tracked_files(); // Load the metadata from the files that have been previously tracked

        // Look for the changes in the key directories and copy them to the backup directory
        for(const std::string& origin : backuped_directories)
            handle_directory_changes_rec(origin, origin, tree_weeks_ago_tm);

        save_tracked_files(); // Save the metadata from the tracked files after the backup
    }

    // Scans and copies modified files into the especified directory where the backup is made
    void handle_directory_changes_rec(const fs::path& origin_dir, const fs::path& current_dir, fs::file_time_type default_track_time) 
    {    
        if (!fs::exists(current_dir) || !fs::is_directory(current_dir))
            return;

        // Check the directory recursively as the modification time of the directory is independent from the
        // internal modification of a file
        for (const fs::directory_entry& entry : fs::directory_iterator(current_dir)) 
        {
            // Process sub-directory content if it is not a lymbolic link and it is not in the ignored directory list
            if(fs::is_directory(entry.path()) && !fs::is_symlink(entry.path()) &&
                ignored_directories.find(entry.path().string()) == ignored_directories.end()) 
            {
                handle_directory_changes_rec(origin_dir, entry.path(), default_track_time); // Recursive directory search
            }
            // Process only the changes in files ignoring the useless files such as temporal ones (filtering by extension)
            else if (fs::is_regular_file(entry.path()) && 
                ignored_extensions.find(entry.path().extension().string()) == ignored_extensions.end()) 
            {
                auto tracked_file_it = tracked_files.find(entry.path().string());
                fs::file_time_type last_modification_t = fs::last_write_time(entry.path());

                fs::file_time_type last_track_time = default_track_time;
                bool file_tracking_is_new = true;

                // Check if the file has being tracked before and has a last modification time
                if(tracked_file_it != tracked_files.end())
                {
                    last_track_time = tracked_file_it->second.last_update;
                    file_tracking_is_new = false;
                }

                // If the file has been modified after the last time or a default time if it was not tracked
                if (last_modification_t > last_track_time) 
                {
                    if(file_tracking_is_new)
                        std::cout << rang::fg::green << "New " << rang::fg::reset;
                    else
                        std::cout << rang::fg::yellow << "Mod " << rang::fg::reset;
                    
                    std::cout << entry.path().string() << "\n";
                    
                    // Make the copy of the file in the backup dir
                    make_file_backup(origin_dir, backup_dir, entry.path());

                    // Save the current time as last actualization of the targeted file
                    FileBackupStatus file_track(entry.path(), fs::file_time_type::clock::now());
                    tracked_files[file_track.path.string()] = file_track;
                }
            }
        }
    }

    // Loads the information about the files that have been tracked previously and cleans deleted ones
    void setup_tracked_files()
    {
        if(!tracked_files.empty())
            tracked_files.clear();

        std::ifstream infile(FILE_TRACK_PATH, std::ifstream::binary);

        if(!infile) return;

        FileBackupStatus file_backup_status;
        
        while (file_backup_status.load(infile)) 
        {
            if(fs::exists(file_backup_status.path))
                tracked_files[file_backup_status.path.string()] = file_backup_status;
            else{
                std::cout << rang::fg::red << "Del " << rang::fg::gray << file_backup_status.path << rang::fg::reset << "\n";
                fs::remove(file_backup_status.path); // Delete file backup of deleted file
            }
        }

        infile.close();
    }

    void save_tracked_files()
    {
        std::ofstream outfile(FILE_TRACK_PATH, std::ofstream::binary);

        if(outfile.fail()) {
            std::cerr << "Error: program unable to save the last files update time\n";
            return;
        }

        for(auto i : tracked_files)
            i.second.save(outfile);

        outfile.close();
    }

    // Creates a copy of a file and its relative path in the backup directory
    void make_file_backup(const fs::path& origin_base_dir, const fs::path& dest_base_dir, const fs::path& saved_file_path) 
    {
        try 
        {
            fs::path relative_path = fs::relative(saved_file_path, origin_base_dir.parent_path());

            // Build the path in the destination directory
            fs::path dest_file_path = dest_base_dir / relative_path;

            // Create the parent directories in the backup directory if they don't exist
            fs::create_directories(dest_file_path.parent_path());

            // Copy the file into the path overwriting it if it existed
            fs::copy_file(saved_file_path, dest_file_path, fs::copy_options::overwrite_existing);
        }
        catch (const fs::filesystem_error& e) {
            std::cerr << "Error al copiar " << saved_file_path.string() << ": " << e.what() << "\n";
        }
    }
};

int main() 
{ 
    std::cout << "\nStating backup...\n\n";
    
    BackupManager bkp_man;

    bkp_man.print_backup_data();

    std::cout << "\nScan results:\n\n";
    
    bkp_man.make_backup();

    std::cout << "\nBackup done :)\n\n";

    std::system("pause");

    return 0;
}
