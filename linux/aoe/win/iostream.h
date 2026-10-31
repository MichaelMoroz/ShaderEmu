// the two files that ask for it print nothing that matters
class ostream { public: template <class T> ostream& operator<<(const T&) { return *this; } };
class istream { public: istream& getline(char*, int, char = 10) { return *this; } int eof() { return 1; } };
class ofstream : public ostream { public: ofstream(const char* = 0, int = 0, int = 0) {} void close() {} };
class ifstream : public istream { public: ifstream(const char* = 0, int = 0, int = 0) {} void close() {} };
static ostream cout, cerr;
static const char endl = 10;
