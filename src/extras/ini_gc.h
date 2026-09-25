#pragma once

// GameCube INI store (B177): the calls re3.cpp makes on mINI, on stdio and one
// heap block per entry. mINI's fstreams linked 444K of libstdc++ ios/locale
// code into MEM1 (b171 -> b172: DOL +534K) to read a 2K settings file.
// Same format: [section], key=value, ';' comments, case-sensitive.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace mINI {

class INIValue
{
	const char *s;
public:
	INIValue(const char *str) : s(str ? str : "") {}
	const char *c_str(void) const { return s; }
};

class INIStructure
{
	struct Entry { char *sec, *key, *val; };   // one malloc: "sec\0key\0val\0"
	Entry *e = nullptr;
	int n = 0, cap = 0;

	int find(const char *sec, const char *key) const {
		for(int i = 0; i < n; i++)
			if(strcmp(e[i].key, key) == 0 && strcmp(e[i].sec, sec) == 0)
				return i;
		return -1;
	}

public:
	void set(const char *sec, const char *key, const char *val) {
		size_t ls = strlen(sec) + 1, lk = strlen(key) + 1, lv = strlen(val) + 1;
		char *b = (char*)malloc(ls + lk + lv);
		if(b == nullptr)
			return;
		memcpy(b, sec, ls); memcpy(b + ls, key, lk); memcpy(b + ls + lk, val, lv);
		int i = find(sec, key);
		if(i >= 0)
			free(e[i].sec);
		else{
			if(n == cap){
				int ncap = cap ? cap*2 : 64;
				Entry *ne = (Entry*)realloc(e, sizeof(Entry)*ncap);
				if(ne == nullptr){ free(b); return; }
				e = ne; cap = ncap;
			}
			i = n++;
		}
		e[i].sec = b; e[i].key = b + ls; e[i].val = b + ls + lk;
	}
	void remove(const char *sec, const char *key) {
		int i = find(sec, key);
		if(i < 0)
			return;
		free(e[i].sec);
		memmove(&e[i], &e[i+1], sizeof(Entry)*(n - i - 1));
		n--;
	}
	void clear(void) {
		for(int i = 0; i < n; i++)
			free(e[i].sec);
		n = 0;
	}

	struct Section {
		const INIStructure *s;
		const char *name;
		bool has(const char *key) const { return s->find(name, key) >= 0; }
		INIValue get(const char *key) const { int i = s->find(name, key); return INIValue(i >= 0 ? s->e[i].val : ""); }
		size_t size(void) const {
			size_t c = 0;
			for(int i = 0; i < s->n; i++)
				c += strcmp(s->e[i].sec, name) == 0;
			return c;
		}
	};
	struct KeyRef {
		INIStructure *s;
		const char *sec, *key;
		KeyRef &operator=(const char *val) { s->set(sec, key, val); return *this; }
	};
	struct SectionRef {
		INIStructure *s;
		const char *name;
		KeyRef operator[](const char *key) { return KeyRef{s, name, key}; }
		void remove(const char *key) { s->remove(name, key); }
	};
	Section get(const char *sec) const { return Section{this, sec}; }
	SectionRef operator[](const char *sec) { return SectionRef{this, sec}; }

	// Sections in first-appearance order, each with its keys.
	bool writeTo(FILE *f) const {
		for(int i = 0; i < n; i++){
			bool first = true;
			for(int j = 0; j < i && first; j++)
				first = strcmp(e[j].sec, e[i].sec) != 0;
			if(!first)
				continue;
			fprintf(f, "[%s]\n", e[i].sec);
			for(int j = i; j < n; j++)
				if(strcmp(e[j].sec, e[i].sec) == 0)
					fprintf(f, "%s=%s\n", e[j].key, e[j].val);
			fputc('\n', f);
		}
		return !ferror(f);
	}
};

class INIFile
{
	const char *path;

	static char *trim(char *s) {
		while(*s == ' ' || *s == '\t') s++;
		char *end = s + strlen(s);
		while(end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
			*--end = '\0';
		return s;
	}

public:
	INIFile(const char *filename) : path(filename) {}

	bool read(INIStructure &data) const {
		data.clear();
		FILE *f = fopen(path, "r");
		if(f == nullptr)
			return false;
		char line[512], sec[64] = "";
		while(fgets(line, sizeof(line), f)){
			char *s = trim(line);
			if(*s == '\0' || *s == ';')
				continue;
			if(*s == '['){
				char *c = strchr(s, ';');
				if(c) *c = '\0';
				char *end = strrchr(s, ']');
				if(end){
					*end = '\0';
					strncpy(sec, trim(s + 1), sizeof(sec) - 1);
					sec[sizeof(sec) - 1] = '\0';
					continue;
				}
			}
			char *eq = strchr(s, '=');
			if(eq == nullptr)
				continue;
			*eq = '\0';
			data.set(sec, trim(s), trim(eq + 1));
		}
		fclose(f);
		return true;
	}
	bool generate(const INIStructure &data, bool = false) const {
		FILE *f = fopen(path, "w");
		if(f == nullptr)
			return false;
		bool ok = data.writeTo(f);
		return fclose(f) == 0 && ok;
	}
	bool write(INIStructure &data, bool pretty = false) const { return generate(data, pretty); }
};

}
