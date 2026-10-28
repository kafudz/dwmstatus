char *
readfile(char *base, char *file)
{
	char *path, line[513];
	FILE *fd;

	memset(line, 0, sizeof(line));

	path = smprintf("%s/%s", base, file);
	fd = fopen(path, "r");
	free(path); /* Pengosongan path dilakukan tepat setelah fopen */
	
	/* PERBAIKAN: Jangan gunakan exit(1). Kembalikan NULL agar program utama tetap berjalan */
	if (fd == NULL) {
		return NULL; 
	}

	if (fgets(line, sizeof(line)-1, fd) == NULL) {
		fclose(fd); /* PERBAIKAN: Tutup fd sebelum keluar untuk mencegah file descriptor leak */
		return NULL;
	}
	fclose(fd);

	return smprintf("%s", line);
}

char *
getbattery(char *base)
{
	char *co;
	int descap = -1;
	int remcap = -1;

	/* 1. Periksa keberadaan baterai */
	co = readfile(base, "present");
	if (co == NULL) {
		return smprintf("n/a");
	}
	if (co[0] != '1') {
		free(co);
		return smprintf("not present");
	}
	free(co);

	/* 2. Ambil kapasitas penuh (Mendukung tipe charge atau energy) */
	co = readfile(base, "charge_full_design");
	if (co == NULL) {
		co = readfile(base, "energy_full_design");
	}
	
	if (co != NULL) {
		sscanf(co, "%d", &descap);
		free(co);
	}

	/* 3. Ambil kapasitas saat ini (Mendukung tipe charge atau energy) */
	co = readfile(base, "charge_now");
	if (co == NULL) {
		co = readfile(base, "energy_now");
	}
	
	if (co != NULL) {
		sscanf(co, "%d", &remcap);
		free(co);
	}

	/* 4. Validasi nilai kalkulasi */
	if (remcap < 0 || descap <= 0) { /* Mencegah pembagian dengan angka 0 */
		return smprintf("invalid");
	}

	return smprintf("%.0f%%", ((float)remcap / (float)descap) * 100);
}
