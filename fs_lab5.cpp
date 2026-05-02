/*
 * Лабораторна робота №5
 * Дисципліна: Системне програмне забезпечення
 * Тема: Файлова система (частина 2)
 * Студент: Білий Іван Віталійович, група ІО-34
 */

#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>
#include <array>
#include <cstring>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <list>

const int BLOCK_SIZE = 128;
const int DIRECT_PTRS = 4; // кількість прямих посилань
const int PTRS_PER_BLOCK = BLOCK_SIZE / sizeof(int); // скільки посилань влізе в один блок

const int TYPE_REG = 0; // звичайний файл
const int TYPE_DIR = 1; // директорія
const int TYPE_SYM = 2; // новий тип для символічних посилань

// фіксований розмір блоку
using Block = std::array<char, BLOCK_SIZE>;

// Дескриптор файлу
struct Inode {
    int id;
    int type;
    int nlink;
    int size;
    int direct[DIRECT_PTRS]; // прямі посилання на блоки
    int indirect;            // непряме посилання
    int open_count;          // скільки разів файл зараз відкрито

    // конструктор за замовчуванням
    Inode() : id(-1), type(TYPE_REG), nlink(0), size(0), indirect(-1), open_count(0) {
        for (int i = 0; i < DIRECT_PTRS; ++i) direct[i] = -1;
    }
    
    // рахую скільки фізичних блоків реально виділено під файл
    int allocated_blocks(const std::unordered_map<int, Block>& disk) const {
        int count = 0;
        for (int i = 0; i < DIRECT_PTRS; ++i) {
            if (direct[i] != -1) count++;
        }
        if (indirect != -1) {
            count++; // враховує сам блок з покажчиками
            if (disk.find(indirect) != disk.end()) {
                const int* ind_ptrs = reinterpret_cast<const int*>(disk.at(indirect).data());
                for (int i = 0; i < PTRS_PER_BLOCK; ++i) {
                    if (ind_ptrs[i] != -1) count++;
                }
            }
        }
        return count;
    }
};

// структура для відкритого файлу
struct OpenFile {
    int inode_id;
    int offset;
    bool is_open;
    OpenFile() : inode_id(-1), offset(0), is_open(false) {}
};

// головний клас нашої ФС
class FileSystem {
private:
    std::unordered_map<int, Block> disk; // оптимізована імітація диску, виділяє пам'ять тільки коли треба
    int next_free_block_id; // лічильник для видачі унікальних ID блокам
    
    std::vector<Inode> inodes;           // масив усіх інодів
    std::vector<bool> inode_bitmap;      // бітова карта вільних інодів
    
    // дерево директорій id_батька -> ім'я_файлу -> id_дитини
    std::unordered_map<int, std::map<std::string, int>> directories;
    
    std::vector<OpenFile> open_files;    // таблиця відкритих файлів
    
    int root_id; // id кореневої директорії /
    int cwd_id;  // id поточної робочої директорії CWD

    // просто видає новий номер
    int alloc_block() {
        return next_free_block_id++;
    }

    // звільняє блок
    void free_block(int bno) {
        disk.erase(bno);
    }

    // шукає вільний inode
    int alloc_inode() {
        for (size_t i = 0; i < inode_bitmap.size(); ++i) {
            if (!inode_bitmap[i]) {
                inode_bitmap[i] = true;
                inodes[i] = Inode();
                inodes[i].id = i;
                return i;
            }
        }
        return -1; 
    }

    // перетворює логічний блок файлу у фізичний якщо allocate=true то виділяє новий
    int get_logical_block(Inode& inode, int lbn, bool allocate) {
        if (lbn < DIRECT_PTRS) {
            if (inode.direct[lbn] == -1 && allocate) {
                inode.direct[lbn] = alloc_block();
            }
            return inode.direct[lbn];
        } else {
            // робота з непрямими посиланнями
            if (inode.indirect == -1) {
                if (!allocate) return -1;
                inode.indirect = alloc_block();
                int* ind_ptrs = reinterpret_cast<int*>(disk[inode.indirect].data());
                for (int i = 0; i < PTRS_PER_BLOCK; ++i) ind_ptrs[i] = -1;
            }
            int ind_idx = lbn - DIRECT_PTRS;
            if (ind_idx >= PTRS_PER_BLOCK) return -1; // файл занадто великий
            
            int* ind_ptrs = reinterpret_cast<int*>(disk[inode.indirect].data());
            if (ind_ptrs[ind_idx] == -1 && allocate) {
                ind_ptrs[ind_idx] = alloc_block();
            }
            return ind_ptrs[ind_idx];
        }
    }

    // очищує всі блоки файлу прямі і непрямі
    void free_file_blocks(Inode& inode) {
        for (int i = 0; i < DIRECT_PTRS; ++i) {
            if (inode.direct[i] != -1) {
                free_block(inode.direct[i]);
                inode.direct[i] = -1;
            }
        }
        if (inode.indirect != -1) {
            if (disk.find(inode.indirect) != disk.end()) {
                int* ind_ptrs = reinterpret_cast<int*>(disk[inode.indirect].data());
                for (int i = 0; i < PTRS_PER_BLOCK; ++i) {
                    if (ind_ptrs[i] != -1) free_block(ind_ptrs[i]);
                }
            }
            free_block(inode.indirect);
            inode.indirect = -1;
        }
    }

    // якщо на файл немає посилань і він закритий то видаляє назавжди
    void check_free_inode(int inode_id) {
        if (inodes[inode_id].nlink <= 0 && inodes[inode_id].open_count == 0) {
            free_file_blocks(inodes[inode_id]);
            directories.erase(inode_id); // чистить вміст, якщо це була директорія
            inode_bitmap[inode_id] = false;
        }
    }

    // парсер шляхів
    int resolve(const std::string& path, int& parent_id, std::string& basename, bool follow_last) {
        // якщо шлях починається з /, йде від кореня інакше від поточної директорії
        int curr = (path.empty() || path[0] != '/') ? cwd_id : root_id;
        parent_id = curr;
        basename = "";

        // розбиваю шлях на компоненти по слешу
        std::list<std::string> parts;
        std::stringstream ss(path);
        std::string item;
        while (std::getline(ss, item, '/')) {
            if (!item.empty() && item != ".") parts.push_back(item);
        }

        int symlinks_followed = 0;

        while (!parts.empty()) {
            std::string part = parts.front();
            parts.pop_front();

            parent_id = curr;
            basename = part;

            if (inodes[curr].type != TYPE_DIR) return -1;

            auto& dir = directories[curr];
            if (dir.find(part) == dir.end()) {
                if (parts.empty()) return -1;
                parent_id = -1;
                return -1; 
            }

            int next_id = dir[part];
            bool is_last = parts.empty();
            
            // якщо натрапив на симлінк і має за ним йти
            if (inodes[next_id].type == TYPE_SYM && (!is_last || follow_last)) {
                if (++symlinks_followed > 10) { // захист від нескінченних циклів
                    std::cout << "Помилка: забагато рiвнiв символiчних посилань\n";
                    return -1;
                }
                
                std::string sym_path = "";
                int bno = inodes[next_id].direct[0];
                if (bno != -1 && disk.find(bno) != disk.end()) {
                    sym_path = std::string(disk[bno].data(), inodes[next_id].size);
                }
                
                // якщо симлінк абсолютний то стрибає в корінь
                if (!sym_path.empty() && sym_path[0] == '/') {
                    curr = root_id;
                }
                
                // розбиваю шлях з симлінку і додаю його на початок черги
                std::vector<std::string> sym_parts;
                std::stringstream ss_sym(sym_path);
                while (std::getline(ss_sym, item, '/')) {
                    if (!item.empty() && item != ".") sym_parts.push_back(item);
                }
                
                for (auto it = sym_parts.rbegin(); it != sym_parts.rend(); ++it) {
                    parts.push_front(*it);
                }
            } else {
                curr = next_id;
            }
        }
        
        if (basename.empty()) {
            parent_id = curr;
            basename = "."; 
        }
        return curr;
    }

public:
    // ініціалізація ФС
    void mkfs(int n) {
        inodes.assign(n, Inode());
        inode_bitmap.assign(n, false);
        disk.clear();
        directories.clear();
        next_free_block_id = 0;
        open_files.assign(256, OpenFile()); 
        
        // створює кореневу директорію /
        root_id = alloc_inode();
        inodes[root_id].type = TYPE_DIR;
        inodes[root_id].nlink = 2; // сама на себе (.) та (..)
        directories[root_id]["."] = root_id;
        directories[root_id][".."] = root_id;
        cwd_id = root_id;
        
        std::cout << "ФС iнiцiалiзована з " << n << " iнодами.\n";
    }

    // створює нову директорію
    void mkdir(const std::string& path) {
        int parent, target; std::string base;
        target = resolve(path, parent, base, false);
        if (target != -1) { std::cout << "Помилка: цiльове iм'я вже iснує\n"; return; }
        if (parent == -1 || base.empty() || base == "." || base == "..") { 
            std::cout << "Помилка: невiрний шлях\n"; return; 
        }

        int id = alloc_inode();
        if (id == -1) { std::cout << "Помилка: немає вiльних iнодiв\n"; return; }
        
        inodes[id].type = TYPE_DIR;
        inodes[id].nlink = 2; 
        directories[id]["."] = id;
        directories[id][".."] = parent;
        directories[parent][base] = id;
        inodes[parent].nlink++; // .. нового нащадка вказує на parent
    }

    // видаляє порожню директорію
    void rmdir(const std::string& path) {
        int parent, target; std::string base;
        target = resolve(path, parent, base, false);
        
        if (target == -1) { std::cout << "Помилка: файл не знайдено\n"; return; }
        if (target == root_id) { std::cout << "Помилка: не можна видалити корiнь\n"; return; }
        
        // забороняє видаляти системні посилання . та ..
        if (base == "." || base == "..") {
            std::cout << "Помилка: не можна видаляти системнi посилання . та ..\n";
            return;
        }

        if (inodes[target].type != TYPE_DIR) { std::cout << "Помилка: це не директорiя\n"; return; }
        if (directories[target].size() > 2) { std::cout << "Помилка: директорiя не порожня\n"; return; }

        directories[parent].erase(base);
        inodes[parent].nlink--; // видаляє посилання ".."
        inodes[target].nlink -= 2; // видаляє "." та зв'язок з батьком
        check_free_inode(target);
    }

    // змінює поточну робочу директорію
    void cd(const std::string& path) {
        int parent, target; std::string base;
        target = resolve(path, parent, base, true);
        if (target == -1) { std::cout << "Помилка: файл не знайдено\n"; return; }
        if (inodes[target].type != TYPE_DIR) { std::cout << "Помилка: це не директорiя\n"; return; }
        cwd_id = target;
    }

    // створює символічне посилання
    void symlink(const std::string& str, const std::string& path) {
        // Захист від переповнення буфера
        if (str.length() > BLOCK_SIZE) {
            std::cout << "Помилка: довжина символiчного посилання перевищує розмiр блоку (" << BLOCK_SIZE << " байт)\n";
            return;
        }

        int parent, target; std::string base;
        target = resolve(path, parent, base, false);
        if (target != -1) { std::cout << "Помилка: цiльове iм'я вже iснує\n"; return; }
        if (parent == -1 || base.empty()) { std::cout << "Помилка: невiрний шлях\n"; return; }

        int id = alloc_inode();
        if (id == -1) { std::cout << "Помилка: немає вiльних iнодiв\n"; return; }
        
        inodes[id].type = TYPE_SYM;
        inodes[id].nlink = 1;
        directories[parent][base] = id;

        // зберігає шлях у першому блоці файлу
        inodes[id].size = str.length();
        int bno = alloc_block();
        inodes[id].direct[0] = bno;
        std::copy(str.begin(), str.end(), disk[bno].begin());
    }

    void stat(const std::string& path) {
        int parent, target; std::string base;
        // stat працює з symlink як lstat
        target = resolve(path, parent, base, false); 
        if (target == -1) { std::cout << "Помилка: файл не знайдено\n"; return; }
        
        const Inode& in = inodes[target];
        std::string t_str = (in.type == TYPE_REG) ? "reg" : (in.type == TYPE_DIR ? "dir" : "sym");
        std::cout << "id=" << target << ", type=" << t_str 
                  << ", nlink=" << in.nlink << ", size=" << in.size 
                  << ", nblock=" << in.allocated_blocks(disk) << "\n";
    }

    void ls(std::string path = "") {
        if (path.empty()) path = ".";
        int parent, target; std::string base;
        target = resolve(path, parent, base, true);
        if (target == -1) { std::cout << "Помилка: файл не знайдено\n"; return; }
        if (inodes[target].type != TYPE_DIR) { std::cout << "Помилка: це не директорiя\n"; return; }

        for (const auto& pair : directories[target]) {
            int id = pair.second;
            std::string t_str = (inodes[id].type == TYPE_REG) ? "reg" : (inodes[id].type == TYPE_DIR ? "dir" : "sym");
            std::cout << pair.first << "\t=> " << t_str << ", " << id;
            
            // якщо це symlink показує куди він вказує
            if (inodes[id].type == TYPE_SYM) {
                int bno = inodes[id].direct[0];
                if (bno != -1 && disk.find(bno) != disk.end()) {
                    std::cout << " -> " << std::string(disk[bno].data(), inodes[id].size);
                }
            }
            std::cout << "\n";
        }
    }

    void create(const std::string& path) {
        int parent, target; std::string base;
        target = resolve(path, parent, base, false);
        if (target != -1) { std::cout << "Помилка: файл вже iснує\n"; return; }
        if (parent == -1 || base.empty()) { std::cout << "Помилка: невiрний шлях\n"; return; }

        int id = alloc_inode();
        if (id == -1) { std::cout << "Помилка: немає вiльних iнодiв\n"; return; }
        
        inodes[id].type = TYPE_REG;
        inodes[id].nlink = 1;
        directories[parent][base] = id;
    }

    void open(const std::string& path) {
        int parent, target; std::string base;
        target = resolve(path, parent, base, true); // відкриває кінцевий файл а не symlink
        if (target == -1) { std::cout << "Помилка: файл не знайдено\n"; return; }
        if (inodes[target].type == TYPE_DIR) { std::cout << "Помилка: не можна вiдкрити директорiю\n"; return; }

        int fd = -1;
        // шукає найменший вільний дескриптор
        for (size_t i = 0; i < open_files.size(); ++i) {
            if (!open_files[i].is_open) { fd = i; break; }
        }
        if (fd == -1) { std::cout << "Помилка: забагато вiдкритих файлiв\n"; return; }
        
        open_files[fd].inode_id = target;
        open_files[fd].offset = 0;
        open_files[fd].is_open = true;
        inodes[target].open_count++;
        std::cout << "fd = " << fd << "\n";
    }

    void close(int fd) {
        if (fd < 0 || fd >= open_files.size() || !open_files[fd].is_open) {
            std::cout << "Помилка: невiрний fd\n"; return;
        }
        int id = open_files[fd].inode_id;
        open_files[fd].is_open = false;
        inodes[id].open_count--;
        check_free_inode(id);
    }

    void seek(int fd, int offset) {
        if (fd < 0 || fd >= open_files.size() || !open_files[fd].is_open) {
            std::cout << "Помилка: невiрний fd\n"; return;
        }
        open_files[fd].offset = offset;
    }

    void read(int fd, int size) {
        if (fd < 0 || fd >= open_files.size() || !open_files[fd].is_open) {
            std::cout << "Помилка: невiрний fd\n"; return;
        }
        int id = open_files[fd].inode_id;
        Inode& in = inodes[id];
        
        int read_bytes = 0;
        std::string result = "";
        
        while (read_bytes < size && open_files[fd].offset < in.size) {
            int lbn = open_files[fd].offset / BLOCK_SIZE;
            int blk_off = open_files[fd].offset % BLOCK_SIZE;
            int pbn = get_logical_block(in, lbn, false);
            
            int to_read = std::min(size - read_bytes, BLOCK_SIZE - blk_off);
            to_read = std::min(to_read, in.size - open_files[fd].offset);
            
            // якщо фізичного блоку немає читає нулі
            if (pbn == -1 || disk.find(pbn) == disk.end()) {
                for (int i = 0; i < to_read; ++i) result += "\\0";
            } else {
                for (int i = 0; i < to_read; ++i) {
                    char c = disk[pbn][blk_off + i];
                    if (c == 0) result += "\\0";
                    else result += c;
                }
            }
            read_bytes += to_read;
            open_files[fd].offset += to_read;
        }
        std::cout << result << "\n";
    }

    void write(int fd, int size, const std::string& data) {
        if (fd < 0 || fd >= open_files.size() || !open_files[fd].is_open) {
            std::cout << "Помилка: невiрний fd\n"; return;
        }
        int id = open_files[fd].inode_id;
        Inode& in = inodes[id];
        
        int written = 0;
        int data_idx = 0;
        
        while (written < size) {
            int lbn = open_files[fd].offset / BLOCK_SIZE;
            int blk_off = open_files[fd].offset % BLOCK_SIZE;
            int pbn = get_logical_block(in, lbn, true); // тут allocate=true
            
            if (pbn == -1) {
                std::cout << "Помилка: файл занадто великий\n"; break;
            }
            
            int to_write = std::min(size - written, BLOCK_SIZE - blk_off);
            for (int i = 0; i < to_write; ++i) {
                disk[pbn][blk_off + i] = (data_idx < data.length()) ? data[data_idx++] : 0;
            }
            
            written += to_write;
            open_files[fd].offset += to_write;
            if (open_files[fd].offset > in.size) {
                in.size = open_files[fd].offset; // оновлює розмір файлу
            }
        }
    }

    void link(const std::string& path1, const std::string& path2) {
        int p1, target1, p2, target2; std::string b1, b2;
        target1 = resolve(path1, p1, b1, false); // link працює з symlink як з файлом
        if (target1 == -1) { std::cout << "Помилка: вихiдний файл не знайдено\n"; return; }
        if (inodes[target1].type == TYPE_DIR) { std::cout << "Помилка: не можна робити link на директорiю\n"; return; }

        target2 = resolve(path2, p2, b2, false);
        if (target2 != -1) { std::cout << "Помилка: цiльове iм'я вже iснує\n"; return; }
        if (p2 == -1 || b2.empty()) { std::cout << "Помилка: невiрний шлях\n"; return; }

        directories[p2][b2] = target1;
        inodes[target1].nlink++;
    }

    void unlink(const std::string& path) {
        int parent, target; std::string base;
        target = resolve(path, parent, base, false); // unlink не слідує за symlink
        if (target == -1) { std::cout << "Помилка: файл не знайдено\n"; return; }
        
        // захист для unlink
        if (base == "." || base == "..") {
            std::cout << "Помилка: не можна видаляти системнi посилання . та ..\n";
            return;
        }
        
        if (inodes[target].type == TYPE_DIR) { std::cout << "Помилка: не можна робити unlink директорiї (використовуйте rmdir)\n"; return; }

        directories[parent].erase(base);
        inodes[target].nlink--;
        check_free_inode(target);
    }

    // оптимізована зміна розміру
    void truncate(const std::string& path, int size) {
        int parent, target; std::string base;
        target = resolve(path, parent, base, true);
        if (target == -1) { std::cout << "Помилка: файл не знайдено\n"; return; }
        if (inodes[target].type == TYPE_DIR) { std::cout << "Помилка: не можна робити truncate директорiї\n"; return; }

        Inode& in = inodes[target];
        // якщо розмір зменшується то чисте зайві фізичні блоки
        if (size < in.size) {
            int new_lbn_end = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
            for (int i = new_lbn_end; i < DIRECT_PTRS; ++i) {
                if (in.direct[i] != -1) { free_block(in.direct[i]); in.direct[i] = -1; }
            }
            if (in.indirect != -1 && disk.find(in.indirect) != disk.end()) {
                int* ind_ptrs = reinterpret_cast<int*>(disk[in.indirect].data());
                bool keep_indirect = false;
                for (int i = 0; i < PTRS_PER_BLOCK; ++i) {
                    if (DIRECT_PTRS + i >= new_lbn_end) {
                        if (ind_ptrs[i] != -1) { free_block(ind_ptrs[i]); ind_ptrs[i] = -1; }
                    } else if (ind_ptrs[i] != -1) {
                        keep_indirect = true;
                    }
                }
                if (!keep_indirect) { free_block(in.indirect); in.indirect = -1; }
            }
        }
        // якщо збільшується то просто міняє in.size блоки не виділяє
        in.size = size;
    }
};

int main() {
    FileSystem fs;
    std::string line;
    
    std::cout << "Iнтерактивна оболонка ФС (Лаб 5)\n> ";
    
    while (std::getline(std::cin, line)) {
        if (line.empty()) { std::cout << "> "; continue; }
        
        std::stringstream ss(line);
        std::string cmd;
        ss >> cmd;
        
        if (cmd == "mkfs") {
            int n; ss >> n; fs.mkfs(n);
        } else if (cmd == "mkdir") {
            std::string path; ss >> path; fs.mkdir(path);
        } else if (cmd == "rmdir") {
            std::string path; ss >> path; fs.rmdir(path);
        } else if (cmd == "cd") {
            std::string path; ss >> path; fs.cd(path);
        } else if (cmd == "symlink") {
            std::string str, path; ss >> str >> path; fs.symlink(str, path);
        } else if (cmd == "create") {
            std::string path; ss >> path; fs.create(path);
        } else if (cmd == "stat") {
            std::string path; ss >> path; fs.stat(path);
        } else if (cmd == "ls") {
            std::string path = ""; 
            if (ss >> path) fs.ls(path);
            else fs.ls();
        } else if (cmd == "link") {
            std::string p1, p2; ss >> p1 >> p2; fs.link(p1, p2);
        } else if (cmd == "unlink") {
            std::string path; ss >> path; fs.unlink(path);
        } else if (cmd == "open") {
            std::string path; ss >> path; fs.open(path);
        } else if (cmd == "close") {
            int fd; ss >> fd; fs.close(fd);
        } else if (cmd == "seek") {
            int fd, off; ss >> fd >> off; fs.seek(fd, off);
        } else if (cmd == "read") {
            int fd, size; ss >> fd >> size; fs.read(fd, size);
        } else if (cmd == "write") {
            int fd, size; ss >> fd >> size;
            char sep; ss.get(sep); // забирає пробіл після size
            std::string data; data.resize(size);
            ss.read(&data[0], size);
            fs.write(fd, size, data);
        } else if (cmd == "truncate") {
            std::string path; int size; ss >> path >> size; fs.truncate(path, size);
        } else if (cmd == "exit") {
            break;
        } else {
            std::cout << "Невiдома команда\n";
        }
        std::cout << "> ";
    }
    return 0;
}
