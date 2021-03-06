#ifndef IMBA_PATH_H
#define IMBA_PATH_H

#include <string>
#include <algorithm>

namespace imba {

class Path {
public:
    Path(const std::string& path)
        : path_(path)
    {
        split();
    }

    Path(const char* path)
        : path_(path)
    {
        split();
    }

    const std::string& path() const { return path_; }
    const std::string& base_name() const { return base_; }
    const std::string& file_name() const { return file_; }

    std::string extension() const {
        auto pos = file_.rfind('.');
        return (pos != std::string::npos) ? file_.substr(pos + 1) : std::string();
    }

    std::string remove_extension() const {
        auto pos = file_.rfind('.');
        return (pos != std::string::npos) ? file_.substr(0, pos) : file_;
    }

    operator const std::string& () const {
        return path();
    }

private:
    std::string path_;
    std::string base_;
    std::string file_;

    void split() {
        std::replace(path_.begin(), path_.end(), '\\', '/');
        auto pos = path_.rfind('/');
        base_ = (pos != std::string::npos) ? path_.substr(0, pos)  : ".";
        file_ = (pos != std::string::npos) ? path_.substr(pos + 1) : path_;
    }
};

} // namespace imba

#endif // IMBA_PATH_H

