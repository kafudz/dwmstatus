/*
 * Contoh penggunaan:
 * gettemperature("/sys/class/hwmon/hwmon0/device", "temp1_input");
 */

char *
gettemperature(char *base, char *sensor)
{
	char *co;
	char *ret;

	co = readfile(base, sensor);
	if (co == NULL)
		return smprintf("");

	/* 1. Alokasikan string hasil konversi suhu terlebih dahulu */
	ret = smprintf("%02.0f°C", atof(co) / 1000);

	/* 2. PERBAIKAN: Bebaskan memori string mentah yang dialokasikan oleh readfile */
	free(co);

	/* 3. Kembalikan string hasil yang sudah aman */
	return ret;
}
