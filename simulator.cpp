#define _CRT_SECURE_NO_WARNINGS
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <mmsystem.h>
#include <fstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#include <commdlg.h>
#include "8080.h"

#pragma comment(lib, "winmm.lib")

extern "C" int cpu8080_step(CPU8080* cpu);
extern "C" void cpu8080_reset(CPU8080* cpu);

#define SYSTEM_CLOCK_HZ 2500000

// --- ПАРАМЕТРЫ ЭКРАНА ОРИОН-128 ---
const int ORION_SCREEN_WIDTH = 384;
const int ORION_SCREEN_HEIGHT = 256;

#define IDM_FILE_OPEN 0x0010
#define IDM_FILE_EXIT 0x0020

// --- ГЛОБАЛЬНЫЕ БУФЕРЫ ПАМЯТИ ---
// 256 Кб ОЗУ в виде 4-х банков по 64 Кб
BYTE orion_ram[4][65536];
// Состояние шин клавиатуры Радио-86РК (Индекс = Столбец PA, Биты = Строки PB)
BYTE key_matrix_state[8];
bool emulator_running = true;

// --- ПОДДЕРЖКА ЗВУКОВОГО КЛИКЕРА ОРИОНА ---
bool last_speaker_bit_state = false;

// --- ВИРТУАЛЬНЫЙ РОМ-ДИСК 64 КБ ---
BYTE rom_disk_storage[65536]; // Буфер для хранения файла "rom.bin"
static DWORD pixel_buffer[384 * 256];

// --- СИСТЕМНЫЕ ПОРТЫ ОРИОН-128 ---
BYTE orion_page = 0;         // Номер текущего банка памяти (Порт 0xF900)
BYTE orion_sys_port1 = 0;    // Управление графическим/цветным режимом (Порт 0xF800)
BYTE orion_sys_port3 = 0;    // Переключение экранной области памяти (Порт 0xFA00)

// Состояния модификаторов клавиатуры РК
bool rk_cc_pressed = false;       // CC (Shift) -> PC5
bool rk_us_pressed = false;       // УС (Ctrl) -> PC6
bool rk_rus_lat_pressed = false;  // РУС/ЛАТ -> PC7

HWND hRegListBox = NULL;

// --- ПАРАМЕТРЫ ЗВУКОВОГО ПОТОКА КР580ВИ53 ---
const int AUDIO_SAMPLE_RATE = 22050;
HWAVEOUT hWaveOut = NULL;
WAVEHDR waveHeader[2];
short* audioBuffers[2] = { NULL, NULL };
const int AUDIO_BUF_SIZE = 1024;

// --- Кольцевой буфер синхронизации INTE ---
const int INTE_RING_BUF_SIZE = 16384;
short inte_ring_buffer[INTE_RING_BUF_SIZE] = { 0 };
volatile int inte_ring_write_ptr = 0;
volatile int inte_ring_read_ptr = 0;

#ifndef PIT_CLOCK_HZ
#define PIT_CLOCK_HZ 2500000   // частота CLK таймера; подставьте значение из схемы вашего Ориона
#endif

static uint16_t pit_bcd2bin(uint16_t v) {
	return (uint16_t)(((v >> 12) & 15) * 1000 + ((v >> 8) & 15) * 100 + ((v >> 4) & 15) * 10 + (v & 15));
}
static uint16_t pit_bin2bcd(uint16_t v) {
	v %= 10000;
	return (uint16_t)(((v / 1000) << 12) | (((v / 100) % 10) << 8) | (((v / 10) % 10) << 4) | (v % 10));
}
static uint16_t pit_norm(uint32_t v, int bcd) { return (uint16_t)(bcd ? v % 10000u : v & 0xFFFFu); }
static uint16_t pit_dec(uint16_t c, int bcd) { return c == 0 ? (uint16_t)(bcd ? 9999 : 0xFFFF) : (uint16_t)(c - 1); }

static void pit_load(PITChannel* ch, uint16_t raw) {
	uint16_t v = ch->bcd ? pit_norm(pit_bcd2bin(raw), 1) : raw;
	ch->reload = v;
	switch (ch->mode) {
	case 0: ch->count = v; ch->armed = true; ch->out = false; ch->state = 0; break;
	case 4: ch->count = v; ch->armed = true; ch->out = true;  ch->state = 0; break;
	case 2: case 3:
		if (!ch->armed) {   // новый счёт подхватывается в конце периода, если счётчик уже идёт
			ch->armed = true; ch->out = true;
			ch->count = (ch->mode == 3) ? pit_norm((uint32_t)v + (v & 1), ch->bcd) : v;
		}
		break;
	default: break;
	}
}

static void pit_write_control(CPU8080* cpu, uint8_t val) {
	cpu->pit_control = val;
	int sel = (val >> 6) & 3;
	if (sel == 3) return;                       // у ВИ53 нет команды Read-Back
	PITChannel* ch = &cpu->pit_channels[sel];
	int rw = (val >> 4) & 3;
	if (rw == 0) {                              // защёлкивание: режим и счёт не затрагиваются
		if (!ch->latched) { ch->latch = ch->count; ch->latched = true; }
		return;
	}
	int m = (val >> 1) & 7;
	if (m >= 6) m -= 4;                         // режимы 6 и 7 - синонимы 2 и 3
	ch->mode = (uint8_t)m;
	ch->bcd = val & 1;
	ch->rw_mode = (uint8_t)rw;
	ch->access_phase = ch->read_phase = 0;
	ch->latched = false; ch->armed = false; ch->state = 0;
	ch->out = (m != 0);
}

static void pit_write_count(PITChannel* ch, uint8_t val) {
	uint16_t v;
	if (ch->rw_mode == 0) return;               // слово режима ещё не записано
	if (ch->rw_mode == 1) v = val;
	else if (ch->rw_mode == 2) v = (uint16_t)(val << 8);
	else if (ch->access_phase == 0) {
		ch->wr_lo = val; ch->access_phase = 1;
		if (ch->mode == 0) { ch->armed = false; ch->out = false; }  // режим 0: счёт стоит до записи MSB
		return;
	}
	else {
		v = (uint16_t)((val << 8) | ch->wr_lo); ch->access_phase = 0;
	}
	pit_load(ch, v);
}

static uint8_t pit_read_count(PITChannel* ch) {
	uint16_t c = ch->latched ? ch->latch : ch->count;
	if (ch->bcd) c = pit_bin2bcd(c);
	uint8_t r;
	if (ch->rw_mode == 2) { r = (uint8_t)(c >> 8); ch->latched = false; }
	else if (ch->rw_mode == 1) { r = (uint8_t)(c & 0xFF); ch->latched = false; }
	else if (ch->read_phase == 0) { r = (uint8_t)(c & 0xFF); ch->read_phase = 1; }
	else { r = (uint8_t)(c >> 8); ch->read_phase = 0; ch->latched = false; }
	return r;
}

static void pit_tick_channel(PITChannel* ch, int n) {
	for (; n > 0 && ch->armed; n--) {
		switch (ch->mode) {
		case 0:
			ch->count = pit_dec(ch->count, ch->bcd);
			if (ch->count == 0) ch->out = true;   // после терминального счёта счётчик продолжает считать
			break;
		case 2:
			ch->count = pit_dec(ch->count, ch->bcd);
			if (ch->count == 1) ch->out = false;
			else if (ch->count == 0) { ch->count = ch->reload; ch->out = true; }
			break;
		case 3:
			ch->count = pit_dec(pit_dec(ch->count, ch->bcd), ch->bcd);
			if (ch->count == 0) {
				ch->out = !ch->out;
				uint32_t r = ch->reload;
				ch->count = pit_norm(ch->out ? r + (r & 1) : (r & ~1u), ch->bcd);
			}
			break;
		case 4:
			ch->count = pit_dec(ch->count, ch->bcd);
			if (ch->state == 1) { ch->out = true; ch->state = 2; }
			else if (ch->state == 0 && ch->count == 0) { ch->out = false; ch->state = 1; }
			break;
		default: return;
		}
	}
}

static void pit_tick(CPU8080* cpu, int n) {
	for (int i = 0; i < 3; i++) pit_tick_channel(&cpu->pit_channels[i], n);
}

void orion128_reset(CPU8080* cpu) {
	orion_page = 0;
	orion_sys_port1 = 0;
	orion_sys_port3 = 0;

	rk_cc_pressed = false;
	rk_us_pressed = false;
	rk_rus_lat_pressed = false;

	for (int i = 0; i < 8; i++) {
		key_matrix_state[i] = 0xFF;
	}

	cpu8080_reset(cpu);
}

// =========================================================================
// ИСПРАВЛЕННЫЙ АППАРАТНЫЙ ДИСПЕТЧЕР ЧТЕНИЯ ПАМЯТИ И ПОРТОВ (ОРИОН-128)
// =========================================================================
extern "C" uint8_t cpu8080_read(CPU8080* cpu, uint16_t addr) {
	// 1. Зона системного Монитора / BIOS (0xF800 - 0xFFFF)
	if (addr >= 0xF800) {
		return orion_ram[0][addr];
	}

	int current_bank = orion_page & 3;

	// 2. Дешифрация аппаратных портов ввода-вывода (0xF400 - 0xF7FF)
	// В Орионе порты доступны только при определенных конфигурациях, проверим стандартное окно:
	if ((current_bank == 0 || current_bank == 1) && addr >= 0xF400 && addr <= 0xF7FF) {

		// К580ВВ55 №1: Порт клавиатуры (0xF400 - 0xF403)
		if ((addr & 0xFF00) == 0xF400) {
			uint16_t reg = addr & 3;
			if (reg == 0) return cpu->ppi0_pa;
			if (reg == 1) {
				uint8_t result_pb = 0xFF;
				for (int col = 0; col < 8; col++) {
					// Если бит в PA сброшен в 0, значит Монитор проверяет этот ряд клавиатуры
					if ((cpu->ppi0_pa & (1 << col)) == 0) {
						result_pb &= key_matrix_state[col];
					}
				}
				// Если Монитор выставил в PA значение 0x00 (проверить все клавиши разом)
				if (cpu->ppi0_pa == 0x00) {
					for (int i = 0; i < 8; i++) {
						result_pb &= key_matrix_state[i];
					}
				}
				return result_pb;
			}
			if (reg == 2) {
				uint8_t result_pc = cpu->ppi0_pc | 0xE0;
				if (rk_cc_pressed) result_pc &= ~(1 << 5);
				if (rk_us_pressed) result_pc &= ~(1 << 6);
				if (rk_rus_lat_pressed) result_pc &= ~(1 << 7);
				return result_pc;
			}
			return cpu->ppi0_ctrl;
		}

		// К580ВВ55 №2: Интерфейс ROM-диска (0xF500 - 0xF503)
		if ((addr & 0xFF00) == 0xF500) {
			uint16_t reg = addr & 3;
			if (reg == 0) {
				uint16_t rom_address = (cpu->ppi1_pc << 8) | cpu->ppi1_pb;
				return rom_disk_storage[rom_address];
			}
			if (reg == 1) return cpu->ppi1_pb;
			if (reg == 2) return cpu->ppi1_pc;
			return cpu->ppi1_ctrl;
		}

		// ППА №3: 0xF600–0xF603
		if ((addr & 0xFF00) == 0xF600) {
			switch (addr & 3) {
			case 0: return cpu->ppi2_pa;
			case 1: return cpu->ppi2_pb;
			case 2: return cpu->ppi2_pc;
			default: return cpu->ppi2_ctrl;
			}
		}

		// Таймер КР580ВИ53: 0xF730–0xF733
		if (addr >= 0xF730 && addr <= 0xF733) {
			uint16_t reg = addr & 3;

			if (reg < 3) return pit_read_count(&cpu->pit_channels[reg]);

			return cpu->pit_control;
		}

		return 0xFF; // Неподключённый адрес окна ввода-вывода: шина "плавает"
	}

	// 3. Служебная область системных переменных Монитора (0xF000 - 0xF3FF)
	if ((current_bank == 0 || current_bank == 1) && addr >= 0xF000 && addr <= 0xF3FF) {
		return orion_ram[0][addr];
	}

	// 4. Чтение из выбранного банка
	return orion_ram[current_bank][addr];
}


// =========================================================================
// ИСПРАВЛЕННЫЙ АППАРАТНЫЙ ДИСПЕТЧЕР ЗАПИСИ В ПАМЯТЬ И ПОРТОВ (ОРИОН-128)
// =========================================================================
extern "C" void cpu8080_write(CPU8080* cpu, uint16_t addr, uint8_t val) {

	// 1. ПЗУ Монитора защищено от записи аппаратно!
	if (addr >= 0xF800) {
		uint16_t port_type = addr & 0xFF00;
		if (port_type == 0xF800) { orion_sys_port1 = val; return; }
		if (port_type == 0xF900) { orion_page = val & 3; return; }
		if (port_type == 0xFA00) { orion_sys_port3 = val & 3; return; }
		return;
	}

	int current_bank = orion_page & 3;

	// 2. Запись в периферийные БИС К580ВВ55 и КР580ВИ53 (0xF400 - 0xF7FF)
	if ((current_bank == 0 || current_bank == 1) && addr >= 0xF400 && addr <= 0xF7FF) {
		if ((addr & 0xFF00) == 0xF400) {
			uint16_t reg = addr & 3;
			if (reg == 0) cpu->ppi0_pa = val;
			else if (reg == 1) cpu->ppi0_pb = val;
			else if (reg == 2) {
				cpu->ppi0_pc = val;
			}
			else {
				cpu->ppi0_ctrl = val;
				if ((val & 0x80) == 0) {
					int bit_to_change = (val >> 1) & 7;
					if (val & 1) cpu->ppi0_pc |= (1 << bit_to_change);
					else cpu->ppi0_pc &= ~(1 << bit_to_change);
				}
			}
			return;
		}
		if ((addr & 0xFF00) == 0xF500) {
			uint16_t reg = addr & 3;
			if (reg == 0) cpu->ppi1_pa = val;
			else if (reg == 1) cpu->ppi1_pb = val;
			else if (reg == 2) cpu->ppi1_pc = val;
			else cpu->ppi1_ctrl = val;
			return;
		}
		if ((addr & 0xFF00) == 0xF600) {
			uint16_t reg = addr & 3;
			if (reg == 0) { cpu->ppi2_pa = val; }
			else if (reg == 1) { cpu->ppi2_pb = val; }
			else if (reg == 2) { cpu->ppi2_pc = val; }
			else cpu->ppi2_ctrl = val;
			return;
		}
		// Системный таймер КР580ВИ53 (PIT)
		if (addr >= 0xF730 && addr <= 0xF733) {
			uint16_t reg = addr & 3;
			if (reg < 3) pit_write_count(&cpu->pit_channels[reg], val);
			else pit_write_control(cpu, val);
			return;
		}
		return;
	}

	if ((current_bank == 0 || current_bank == 1) && addr >= 0xF000 && addr <= 0xF3FF) {
		orion_ram[0][addr] = val;
		return;
	}

	orion_ram[current_bank][addr] = val;
}

static CPU8080 cpu;

// --- ЗАГРУЗКА И КОРРЕКТИРОВКА РОМОВ ---
bool LoadRawBinaryFile(const wchar_t* filepath, BYTE* target_dest, size_t max_bytes) {
	std::ifstream file(filepath, std::ios::binary);
	if (!file.is_open()) return false;
	file.read(reinterpret_cast<char*>(target_dest), max_bytes);
	file.close();
	return true;
}

// --- ИСПРАВЛЕННАЯ ЗАГРУЗКА ПЗУ ---
void LoadRoms() {
	orion128_reset(&cpu);
	// ИСПРАВЛЕНО: Загружаем Монитор точно по его физическому адресу 0xF800 в Банке 0
	if (!LoadRawBinaryFile(L"monitor.bin", &orion_ram[0][0xF800], 2048)) {
		MessageBoxW(NULL, L"Не удалось открыть файл системного монитора monitor.bin!", L"Предупреждение", MB_OK | MB_ICONWARNING);
	}

	// ДОБАВЛЕНО: Загрузка РОМ-диска 64 Кб для Порта Пользователя №1
	memset(rom_disk_storage, 0xFF, sizeof(rom_disk_storage)); // Инициализация чистой памяти
	if (!LoadRawBinaryFile(L"romdisk.bin", rom_disk_storage, 65536)) {
		MessageBoxW(NULL, L"Не удалось открыть файл РОМ-диска romdisk.bin!\nБудет смонтирован пустой диск.", L"Информация", MB_OK | MB_ICONINFORMATION);
	}

	for (int i = 0; i < 8; i++) key_matrix_state[i] = 0xFF;
}

void UpdateRegisterDisplay() {
	SendMessageW(hRegListBox, LB_RESETCONTENT, 0, 0);
	wchar_t buf[64];
	swprintf(buf, 64, L" PC: %04Xh  SP: %04Xh", cpu.PC, cpu.SP);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);

	// Показываем текущий банк процессора и физический банк видеовыхода
	swprintf(buf, 64, L" REG A: %02Xh  Банк ОЗУ: %d", cpu.A, orion_page);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);

	swprintf(buf, 64, L" REG B: %02Xh  REG C: %02Xh", cpu.B, cpu.C);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
	swprintf(buf, 64, L" REG D: %02Xh  REG E: %02Xh", cpu.D, cpu.E);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
	swprintf(buf, 64, L" REG H: %02Xh  REG L: %02Xh", cpu.H, cpu.L);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);

	swprintf(buf, 64, L" Флаги: S:%d Z:%d AC:%d P:%d CY:%d", cpu.S, cpu.Z, cpu.AC, cpu.P, cpu.CY);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);

	// Расширенная видео-отладка
	WORD screen_addr = 0xC000;
	switch (orion_sys_port3 & 3) {
	case 0: screen_addr = 0xC000; break;
	case 1: screen_addr = 0x8000; break;
	case 2: screen_addr = 0x4000; break;
	case 3: screen_addr = 0x0000; break;
	}

	swprintf(buf, 64, L" Экран: №%d (Адрес: %04Xh)", orion_sys_port3 + 1, screen_addr);
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);

	// Битовая расшифровка Порта 1
	bool mode_c = (orion_sys_port1 & 0x04) != 0;
	bool mode_2 = (orion_sys_port1 & 0x02) != 0;
	swprintf(buf, 64, L" Порт1: %02Xh (%s, %s)",
		orion_sys_port1,
		mode_c ? L"Цветной" : L"Монохром",
		mode_2 ? L"16-цв (D1=1)" : L"4-цв (D1=0)");
	SendMessageW(hRegListBox, LB_ADDSTRING, 0, (LPARAM)buf);
}

// =========================================================================
// ПОЛНАЯ ИСПРАВЛЕННАЯ ФУНКЦИЯ ПОПИКСЕЛЬНОГО ВЫВОДА ЭКРАНА ОРИОН-128 (384х256)
// =========================================================================
void RenderScreen(HDC hdc, int xOffset, int yOffset) {
	// Эталонная 16-цветная палитра Ориона-128 (В формате BGR для StretchDIBits)
	static const DWORD palette16[] = {
		0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA, 0x00AA0000, 0x00AA00AA,
		0x00AA5500, 0x00AAAAAA, 0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
		0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF
	};

	// Определение стартового адреса текущего экрана на основе Системного порта №3
	WORD screen_base = 0xC000;
	switch (orion_sys_port3 & 3) {
	case 0: screen_base = 0xC000; break;
	case 1: screen_base = 0x8000; break;
	case 2: screen_base = 0x4000; break;
	case 3: screen_base = 0x0000; break;
	}

	// Дешифрация режима цветности строго по спецификации вашего проекта
	bool color_mode = (orion_sys_port1 & 0x04) != 0;    // Бит D2: 1 - Цвет, 0 - Монохром

	// СТРОГО ПО СПЕЦИФИКАЦИИ: 
	// Если Бит D1 == 0 (как в Порт1 = 24h) -> это 4-цветный игровой режим.
	// Если Бит D1 == 1 (как в Порт1 = 07h) -> это 16-цветный режим ОРДОС.
	bool mode_4_color = (orion_sys_port1 & 0x02) == 0;

	BYTE palette_idx = orion_sys_port1 & 0x01;          // Бит D0: Палитра

	// ДИНАМИЧЕСКИЙ ВЫБОР СТРАНИЦ ВИДЕОПАМЯТИ ДЛЯ РЕНДЕРИНГА (СТРОГО С ВАШЕЙ СТРАНИЦЫ 14)
	int bank_mask = 0;
	int bank_color = 1;
	// Экранные страницы переключаются битом D3 порта 0xF800 (orion_sys_port1)
	if ((orion_sys_port1 & 0x08) != 0) {
		bank_mask = 2;  // Графическая маска перемещается в Банк 2
		bank_color = 3; // Атрибуты цвета перемещаются в Банк 3
	}

	// 1. МОНОХРОМНЫЙ РЕЖИМ РАБОТЫ ЭКРАНА (Бит D2 = 0)
	if (!color_mode) {
		DWORD color_on = (palette_idx == 0) ? 0x0000FF00 : 0x00FFFF00; // Зеленый или Желтый текст (DIB: 0x00RRGGBB)
		DWORD color_off = 0x00000000; // Черный фон
		for (int x_byte = 0; x_byte < 48; x_byte++) {
			for (int y = 0; y < 256; y++) {
				WORD addr = screen_base + (x_byte * 256) + y;
				BYTE pixels = orion_ram[bank_mask][addr];
				int pixel_y = (256 - 1) - y;
				for (int bit = 0; bit < 8; bit++) {
					int dot = 7 - bit;
					int pixel_x = (x_byte * 8) + bit;
					bool dot_state = (pixels & (1 << dot)) != 0;
					pixel_buffer[pixel_y * 384 + pixel_x] = dot_state ? color_on : color_off;
				}
			}
		}
	}
	// 2. ЦВЕТНЫЕ РЕЖИМЫ РАБОТЫ (Бит D2 = 1)
	else {
		for (int x_byte = 0; x_byte < 48; x_byte++) {
			for (int y = 0; y < 256; y++) {
				WORD addr = screen_base + (x_byte * 256) + y;

				BYTE layer0 = orion_ram[bank_mask][addr];  // Слой маски (Банк 0 или 2)
				BYTE layer1 = orion_ram[bank_color][addr]; // Слой цвета/атрибутов (Банк 1 или 3)
				int pixel_y = (256 - 1) - y;

				if (mode_4_color) {
					// ИГРОВОЙ 4-ЦВЕТНЫЙ РЕЖИМ (Побитная склейка двух слоев, Порт1 = 24h)
					for (int bit = 0; bit < 8; bit++) {
						int dot = 7 - bit;
						int bit0 = (layer0 & (1 << dot)) ? 1 : 0;
						int bit1 = (layer1 & (1 << dot)) ? 2 : 0;
						int color_code = bit0 | bit1;

						// Маппинг 4 цветов на палитру palette16
						BYTE final_idx = 0;
						if (palette_idx == 0) {
							static const BYTE pal1[] = { 0, 4, 2, 1 }; // черный, красный, зеленый, синий
							final_idx = pal1[color_code];
						}
						else {
							static const BYTE pal2[] = { 0, 5, 3, 15 };
							final_idx = pal2[color_code];
						}

						DWORD pixel_color = palette16[final_idx & 0x0F];
						int pixel_x = x_byte * 8 + bit;
						if (pixel_x >= 0 && pixel_x < 384 && pixel_y >= 0 && pixel_y < 256) {
							pixel_buffer[pixel_y * 384 + pixel_x] = pixel_color;
						}
					}
				}
				else {
					// СИСТЕМНЫЙ 16-ЦВЕТНЫЙ РЕЖИМ ОРДОС (Групповое кодирование тетрадами, Порт1 = 07h)
					BYTE bg_idx = (layer1 >> 4) & 0x0F; // Старшая тетрада — цвет фона
					BYTE fg_idx = layer1 & 0x0F;        // Младшая тетрада — цвет текста

					for (int bit = 0; bit < 8; bit++) {
						int dot = 7 - bit;
						bool bit0 = (layer0 & (1 << dot)) != 0;
						BYTE final_idx = bit0 ? fg_idx : bg_idx;

						DWORD pixel_color = palette16[final_idx];
						int pixel_x = x_byte * 8 + bit;
						if (pixel_x >= 0 && pixel_x < 384 && pixel_y >= 0 && pixel_y < 256) {
							pixel_buffer[pixel_y * 384 + pixel_x] = pixel_color;
						}
					}
				}
			}
		}
	}

	// Инициализация структуры метаданных кадра для Windows API
	BITMAPINFO bmi = { 0 };
	bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth = 384;
	bmi.bmiHeader.biHeight = 256;
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	// Вывод кадра в окно Windows с масштабированием x2
	StretchDIBits(hdc, xOffset, yOffset, 384 * 2, 256 * 2, 0, 0, 384, 256, pixel_buffer, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

#ifndef BS_NOFOCUS
#define BS_NOFOCUS 0x00008000L
#endif

// =========================================================================
// ФУНКЦИЯ ДИНАМИЧЕСКОЙ ГЕНЕРАЦИИ ИНТЕРФЕЙСА КЛАВИАТУРЫ (РАДИО-86РК)
// =========================================================================
void CreateKeyboardUI(HWND hwndParent, HINSTANCE hInst) {
	const int BTN_W = 40; const int BTN_H = 40; const int START_X = 20;
	const int START_Y = 560; const int GAP = 4;

	static const wchar_t* main_labels[] = {
		L";\n+", L"1\n!", L"2\n\"", L"3\n#", L"4\n$", L"5\n%", L"6\n&", L"7\n'",
		L"8\n(", L"9\n)", L"0", L"-\n=", L"ТАБ", L"ПС", L"",
		L"Й\nJ", L"Ц\nC", L"У\nU", L"К\nK", L"Е\nE", L"Н\nN", L"Г\nG", L"Ш\n[",
		L"Щ\n]", L"З\nZ", L"Х\nH", L":\n*", L"ВК", L"", L"",
		L"УС", L"Ф\nF", L"Ы\nY", L"В\nW", L"А\nA", L"П\nP", L"Р\nR", L"О\nO",
		L"Л\nL", L"Д\nD", L"Ж\n\\V", L"Э\n\\", L".\n>", L"ЗБ", L"",
		L"СС", L"Я\nQ", L"Ч\n^", L"С\nS", L"М\nM", L"И\nI", L"Т\nT", L"Ь\nX",
		L"Б\nB", L"Ю\n@", L",\n<", L"/\n?", L"РУС\nЛАТ", L"[ _ ]", L"[ _ ]"
	};

	static const wchar_t* side_labels[] = { L"↖", L"F1", L"СТР", L"←", L"↑", L"→", L"F2", L"F3", L"F4", L"↓", L"AP2", L"" };

	for (int idx = 0; idx < 60; idx++) {
		int r = idx / 15; int c = idx % 15; int current_id = 1000 + idx;
		if (idx == 14) continue; // Пропуск отсутствующей кнопки
		if ((r == 1 && c >= 13) || (r == 2 && c == 14)) continue;

		int x = START_X + c * (BTN_W + GAP); int y = START_Y + r * (BTN_H + GAP);

		if (r == 3 && c == 13) { // Пробел
			CreateWindowExW(0, L"BUTTON", main_labels[idx],
				WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE | BS_FLAT | BS_NOFOCUS,
				x, y, (BTN_W * 2) + GAP, BTN_H, hwndParent, (HMENU)(INT_PTR)current_id, hInst, NULL);
			continue;
		}
		if (r == 3 && c == 14) continue;

		CreateWindowExW(0, L"BUTTON", main_labels[idx],
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE | BS_FLAT | BS_NOFOCUS,
			x, y, BTN_W, BTN_H, hwndParent, (HMENU)(INT_PTR)current_id, hInst, NULL);
	}

	int SIDE_START_X = START_X + 15 * (BTN_W + GAP) + 15;
	for (int side_idx = 0; side_idx < 12; side_idx++) {
		int r = side_idx / 3; int c = side_idx % 3; int current_id = 1060 + side_idx;
		if (r == 3 && c == 2) continue;

		int x = SIDE_START_X + c * (BTN_W + GAP); int y = START_Y + r * (BTN_H + GAP);

		CreateWindowExW(0, L"BUTTON", side_labels[side_idx],
			WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_MULTILINE | BS_FLAT | BS_NOFOCUS,
			x, y, BTN_W, BTN_H, hwndParent, (HMENU)(INT_PTR)current_id, hInst, NULL);
	}
}

static const int rk_hardware_map[] = {
    // 0 - 14 (Строка 1 экранной клавиатуры: ;+, 1!, 2", ..., ТАБ, ПС)
    3 * 8 + 3, 2 * 8 + 1, 2 * 8 + 2, 2 * 8 + 3, 2 * 8 + 4, 2 * 8 + 5, 2 * 8 + 6, 2 * 8 + 7, 3 * 8 + 0, 3 * 8 + 1, 2 * 8 + 0, 3 * 8 + 5, 1 * 8 + 0, 1 * 8 + 1, 0,
    
    // 15 - 29 (Строка 2 экранной клавиатуры: Й, Ц, У, К, ..., ВК)
    5 * 8 + 2, 4 * 8 + 3, 6 * 8 + 5, 5 * 8 + 3, 4 * 8 + 5, 5 * 8 + 6, 4 * 8 + 7, 7 * 8 + 3, 7 * 8 + 5, 7 * 8 + 2, 5 * 8 + 0, 3 * 8 + 2, 1 * 8 + 2, 0, 0,
    
    // 30 - 44 (Строка 3 экранной клавиатуры: УС, Ф, Ы, В, ..., ЗБ)
    0, 4 * 8 + 6, 7 * 8 + 1, 6 * 8 + 7, 4 * 8 + 1, 6 * 8 + 0, 6 * 8 + 2, 5 * 8 + 7, 5 * 8 + 4, 4 * 8 + 4, 6 * 8 + 6, 7 * 8 + 4, 3 * 8 + 6, 1 * 8 + 3, 0,
    
    // 45 - 59 (Строка 4 экранной клавиатуры: СС, Я, Ч, С, ..., ПРОБЕЛ)
    0, 6 * 8 + 1, 7 * 8 + 6, 6 * 8 + 3, 5 * 8 + 5, 5 * 8 + 1, 6 * 8 + 4, 7 * 8 + 0, 4 * 8 + 2, 4 * 8 + 0, 3 * 8 + 4, 3 * 8 + 7, 0, 7 * 8 + 7, 7 * 8 + 7,
    
    // 60 - 71 (Правый боковой блок: СТРЕЛКИ, F1-F4, СТР, АР2, "В угол")
    0 * 8 + 0, 0 * 8 + 3, 0 * 8 + 1, 1 * 8 + 4, 1 * 8 + 5, 1 * 8 + 6, 0 * 8 + 4, 0 * 8 + 5, 0 * 8 + 6, 1 * 8 + 7, 0 * 8 + 2, 0
};

bool MapVirtualKeyToRK(UINT vkCode, int& out_row, int& out_col) {
	int target_idx = -1;
	switch (vkCode) {
		// --- ЦИФРОВОЙ РЯД ПК ---
	case 0xBB: target_idx = 26;  break; // Клавиша ` / Ё  -> мапим на [;] (Index 0)
	case '1':  target_idx = 1;  break;
	case '2':  target_idx = 2;  break;
	case '3':  target_idx = 3;  break;
	case '4':  target_idx = 4;  break;
	case '5':  target_idx = 5;  break;
	case '6':  target_idx = 6;  break;
	case '7':  target_idx = 7;  break;
	case '8':  target_idx = 8;  break;
	case '9':  target_idx = 9;  break;
	case '0':  target_idx = 10; break;
	case 0xBD: target_idx = 11; break; // Клавиша минус [-] -> мапим на [-] (Index 11)

		// --- БУКВЕННЫЕ КЛАВИШИ: СИМВОЛЬНОЕ СООТВЕТСТВИЕ QWERTY ---
		// Ищем в main_labels, на какой кнопке РК находится нужная латинская буква
	case 'Q': target_idx = 46; break; // Нажимаем ПК 'Q' -> РК выдает 'Q' (Кнопка Я/Q, Index 46)
	case 'W': target_idx = 33; break; // Нажимаем ПК 'W' -> РК выдает 'W' (Кнопка В/W, Index 33)
	case 'E': target_idx = 19; break; // Нажимаем ПК 'E' -> РК выдает 'E' (Кнопка Е/E, Index 19)
	case 'R': target_idx = 36; break; // Нажимаем ПК 'R' -> РК выдает 'R' (Кнопка Р/R, Index 36)
	case 'T': target_idx = 51; break; // Нажимаем ПК 'T' -> РК выдает 'T' (Кнопка Т/T, Index 51)
	case 'Y': target_idx = 32; break; // Нажимаем ПК 'Y' -> РК выдает 'Y' (Кнопка Ы/Y, Index 32)
	case 'U': target_idx = 17; break; // Нажимаем ПК 'U' -> РК выдает 'U' (Кнопка У/U, Index 17)
	case 'I': target_idx = 50; break; // Нажимаем ПК 'I' -> РК выдает 'I' (Кнопка И/I, Index 50)
	case 'O': target_idx = 37; break; // Нажимаем ПК 'O' -> РК выдает 'O' (Кнопка О/O, Index 37)
	case 'P': target_idx = 35; break; // Нажимаем ПК 'P' -> РК выдает 'P' (Кнопка П/P, Index 35)

	case 'A': target_idx = 34; break; // Нажимаем ПК 'A' -> РК выдает 'A' (Кнопка А/A, Index 34)
	case 'S': target_idx = 48; break; // Нажимаем ПК 'S' -> РК выдает 'S' (Кнопка С/S, Index 48)
	case 'D': target_idx = 39; break; // Нажимаем ПК 'D' -> РК выдает 'D' (Кнопка Д/D, Index 39)
	case 'F': target_idx = 31; break; // Нажимаем ПК 'F' -> РК выдает 'F' (Кнопка Ф/F, Index 31)
	case 'G': target_idx = 21; break; // Нажимаем ПК 'G' -> РК выдает 'G' (Кнопка Г/G, Index 21)
	case 'H': target_idx = 25; break; // Нажимаем ПК 'H' -> РК выдает 'H' (Кнопка Х/H, Index 25)
	case 'J': target_idx = 15; break; // Нажимаем ПК 'J' -> РК выдает 'J' (Кнопка Й/J, Index 15)
	case 'K': target_idx = 18; break; // Нажимаем ПК 'K' -> РК выдает 'K' (Кнопка К/K, Index 18)
	case 'L': target_idx = 38; break; // Нажимаем ПК 'L' -> РК выдает 'L' (Кнопка Л/L, Index 38)

	case 'Z': target_idx = 24; break; // Нажимаем ПК 'Z' -> РК выдает 'Z' (Кнопка З/Z, Index 24)
	case 'X': target_idx = 52; break; // Нажимаем ПК 'X' -> РК выдает 'X' (Кнопка Ь/X, Index 52)
	case 'C': target_idx = 16; break; // Нажимаем ПК 'C' -> РК выдает 'C' (Кнопка Ц/C, Index 16)
	case 'V': target_idx = 40; break; // Нажимаем ПК 'V' -> РК выдает 'V' (Кнопка Ж/\V, Index 40)
	case 'B': target_idx = 53; break; // Нажимаем ПК 'B' -> РК выдает 'B' (Кнопка Б/B, Index 53)
	case 'N': target_idx = 20; break; // Нажимаем ПК 'N' -> РК выдает 'N' (Кнопка Н/N, Index 20)
	case 'M': target_idx = 49; break; // Нажимаем ПК 'M' -> РК выдает 'M' (Кнопка М/M, Index 49)

		// --- ПУНКТУАЦИЯ И СПЕЦСИМВОЛЫ ---
	case 0xDB: target_idx = 22; break; // Клавиша [ { -> мапим на Ш / [ (Index 22)
	case 0xDD: target_idx = 23; break; // Клавиша ] } -> мапим на Щ / ] (Index 23)
	case 0xBA: target_idx = 54; break; // Клавиша ; : -> мапим на : / * (Index 26)
	case 0xDE: target_idx = 47; break; // Клавиша ' " -> мапим на Э / \ (Index 41)
	case 0xDC: target_idx = 41; break; // Клавиша ' " -> мапим на Э / \ (Index 41)
	case 0xC0: target_idx = 0; break; // Клавиша ' " -> мапим на Э / \ (Index 41)
	case 0xBC: target_idx = 55; break; // Клавиша , < -> мапим на РК-шную запятую (Index 55)
	case 0xBE: target_idx = 42; break; // Клавиша . > -> мапим на . / > (Index 42)
	case 0xBF: target_idx = 56; break; // Клавиша / ? -> мапим на РК-шный слэш (Index 56)

	case VK_RETURN: target_idx = 27; break; // ВК (Enter)
	case VK_BACK:   target_idx = 43; break; // ЗБ (Backspace)
	case VK_SPACE:  target_idx = 58; break; // ПРОБЕЛ

		// --- УПРАВЛЯЮЩИЙ БЛОК ПК И СТРЕЛКИ ---
	case VK_F1:    target_idx = 61; break; // Кнопка F1
	case VK_F2:    target_idx = 66; break; // Кнопка F2
	case VK_F3:    target_idx = 67; break; // Кнопка F3
	case VK_F4:    target_idx = 68; break; // Кнопка F4
	case VK_LEFT:  target_idx = 63; break;
	case VK_UP:    target_idx = 64; break;
	case VK_RIGHT: target_idx = 65; break;
	case VK_DOWN:  target_idx = 69; break;
	case VK_HOME:  target_idx = 60; break; // "в угол"
	case VK_PRIOR:  target_idx = 62; break; // СТР (Page Up)
	}

	if (target_idx != -1) {
		int hw_code = rk_hardware_map[target_idx];
		out_row = hw_code / 8; out_col = hw_code % 8;
		return true;
	}
	return false;
}

// Автоматический поиск смещения для записи нового файла в RAM-диск B (orion_ram[1])
WORD GetNextOrdosOffset() {
	WORD offset = 0;
	const WORD RAMDISK_TOP = 0xF000; // Верхняя граница рамдиска под системные нужды

	while (offset < RAMDISK_TOP - 16) {
		// Если встретили маркер пустого пространства или конца цепочки
		if (orion_ram[1][offset] == 0xFF || orion_ram[1][offset] == 0x00) {
			return offset;
		}

		// Читаем длину данных из ORDOS-заголовка текущего файла (смещение 10, 11)
		WORD datasize = orion_ram[1][offset + 10] | (orion_ram[1][offset + 11] << 8);

		// Защита от зацикливания при поврежденном каталоге
		if (datasize == 0 || (offset + 16 + datasize) > RAMDISK_TOP) {
			return offset;
		}

		offset += 16 + datasize;
	}
	return offset;
}

void LoadOrionFile(HWND hwnd) {
	OPENFILENAMEW ofn;
	wchar_t szFile[1024] = { 0 }; // Буфер увеличен для поддержки Multi-Select без STD
	ZeroMemory(&ofn, sizeof(ofn));
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = hwnd;
	ofn.lpstrFile = szFile;
	ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
	ofn.lpstrFilter = L"Файлы Ориона (*.ord;*.rko;*.bru;*.bin)\0*.ord;*.rko;*.bru;*.bin\0Все файлы (*.*)\0*.*\0";
	ofn.nFilterIndex = 1;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

	if (GetOpenFileNameW(&ofn) == TRUE) {
		// Указатель для ручного разбора путей Multi-Select (строки разделены '\0')
		wchar_t* ptr = ofn.lpstrFile;
		wchar_t dir[512] = { 0 };
		wcscpy(dir, ptr);
		ptr += wcslen(dir) + 1;

		// Если выбран один файл, ptr сразу будет указывать на конец буфера. 
		// Если несколько — dir содержит общую папку, а ptr последовательно имена файлов.
		bool is_multi = (*ptr != L'\0');
		int loaded_count = 0;

		do {
			wchar_t full_path[512] = { 0 };
			if (is_multi) {
				swprintf(full_path, 512, L"%s\\%s", dir, ptr);
				ptr += wcslen(ptr) + 1;
			}
			else {
				wcscpy(full_path, dir);
			}

			// Извлекаем расширение файла вручную (в нижнем регистре)
			wchar_t ext[8] = { 0 };
			wchar_t* dot = wcsrchr(full_path, L'.');
			if (dot) {
				int i = 0;
				while (dot[i] && i < 7) {
					ext[i] = towlower(dot[i]);
					i++;
				}
			}

			// Проверяем: если это обычный бинарник .BIN, обрабатываем по старому алгоритму
			if (wcscmp(ext, L".bin") == 0) {
				std::ifstream file(full_path, std::ios::binary);
				if (file.is_open()) {
					file.seekg(0, std::ios::end);
					size_t file_size = file.tellg();
					file.seekg(0, std::ios::beg);

					if (file_size == 0 || file_size == (size_t)-1) {
						file.close();
						MessageBoxW(hwnd, L"Файл пуст или его размер не определён.", L"Ошибка", MB_OK | MB_ICONERROR);
						return;
					}

					WORD load_address = 0x0000;
					WORD file_length = (file_size > 48 * 1024) ? 48 * 1024 : (WORD)file_size;

					// Читаем исполняемый код программы строго в БАНК 0 (как было раньше)
					file.read(reinterpret_cast<char*>(&orion_ram[0][load_address]), file_length);
					file.close();

					// Полный аппаратный сброс и подготовка контекста под запуск игры
					orion_page = 0;
					orion_sys_port1 = 0x24; // Форсируем игровой цветной режим 4-цв
					orion_sys_port3 = 0;
					cpu.EI = false;
					cpu.halted = false;          // иначе после прежнего HLT программа не стартует
					cpu.int_pending = false;
					cpu.PC = load_address;
					cpu.SP = 0xBFFF;             // ниже экранной памяти (C000-EFFF), чтобы стек не портил картинку

					// Заглушка под ORDOS (RET) - только если она не затирает данные самой программы
					if (file_length <= 0xBE03) orion_ram[0][0xBE03] = 0xC9;
					for (int o_addr = 0xF000; o_addr < 0xF800; o_addr++) {
						orion_ram[0][o_addr] = 0x00;
					}
					for (int i = 0; i < 8; i++) key_matrix_state[i] = 0xFF;

					UpdateRegisterDisplay();
					InvalidateRect(hwnd, NULL, FALSE);

					wchar_t msg_buf[256];
					swprintf(msg_buf, 256, L"Бинарный файл успешно запущен!\nАдрес старта: %04Xh\nРазмер: %d байт", load_address, file_length);
					MessageBoxW(hwnd, msg_buf, L"Успех", MB_OK | MB_ICONINFORMATION);
				}
				return; // .BIN файлы обрабатываются одиночно, прерываем функцию
			}

			// --- ОБРАБОТКА ДЛЯ ФОРМАТОВ ORDOS (.ORD, .RKO, .BRU) ---
			// Выводим предупреждение перед первой записью на диск B
			if (loaded_count == 0) {
				if (MessageBoxW(hwnd, L"Данные на RAM-диске B (страница 1) будут изменены.\nПродолжить?",
					L"Внимание: Загрузка в RAM-диск", MB_OKCANCEL | MB_ICONEXCLAMATION) != IDOK) {
					return;
				}
			}

			std::ifstream file(full_path, std::ios::binary);
			if (file.is_open()) {
				file.seekg(0, std::ios::end);
				size_t file_size = file.tellg();
				file.seekg(0, std::ios::beg);

				bool is_rko = (wcscmp(ext, L".rko") == 0);
				if (is_rko) {
					if (file_size > 77) {
						file.seekg(77, std::ios::beg);
						file_size -= 77;
					}
					else {
						file.close();
						continue;
					}
				}

				const WORD RAMDISK_TOP = 0xF000;
				WORD next_ordos = GetNextOrdosOffset();

				if (next_ordos + 16 < RAMDISK_TOP) {
					BYTE ordos_header[16];
					if (file.read(reinterpret_cast<char*>(ordos_header), 16)) {
						WORD datasize = ordos_header[10] | (ordos_header[11] << 8);

						if (next_ordos + 16 + datasize < RAMDISK_TOP) {
							// Копируем заголовок на страницу 1 (Диск B)
							memcpy(&orion_ram[1][next_ordos], ordos_header, 16);
							// Читаем данные следом за заголовком
							file.read(reinterpret_cast<char*>(&orion_ram[1][next_ordos + 16]), datasize);

							next_ordos += 16 + datasize;
							orion_ram[1][next_ordos] = 0xFF; // Конец цепочки файлов
							loaded_count++;
						}
					}
				}
				file.close();
			}

		} while (is_multi && *ptr != L'\0');

		if (loaded_count > 0) {
			UpdateRegisterDisplay();
			InvalidateRect(hwnd, NULL, FALSE);

			wchar_t msg_buf[256];
			swprintf(msg_buf, 256, L"Загружено файлов на диск B: %d.\nПерезапустите Оболочку/Командер (кнопка F4).", loaded_count);
			MessageBoxW(hwnd, msg_buf, L"Информация", MB_OK | MB_ICONINFORMATION);
		}
	}
}

int static_virtual_key_duration = 0; // Наш счетчик автоотжатия кнопок мыши
const int HARDWARE_MAP_SIZE = sizeof(rk_hardware_map) / sizeof(rk_hardware_map[0]);

// =========================================================================
// ОКОННАЯ ПРОЦЕДУРА WINDOWS ДЛЯ СИМУЛЯТОРА
// =========================================================================
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	static HINSTANCE hInst; switch (msg) {
	case WM_CREATE: {
		LPCREATESTRUCTW pcs = (LPCREATESTRUCTW)lp;
		hInst = pcs->hInstance;
		HMENU hSysMenu = GetSystemMenu(hwnd, FALSE);
		if (hSysMenu) {
			AppendMenuW(hSysMenu, MF_SEPARATOR, 0, NULL);
			AppendMenuW(hSysMenu, MF_STRING, IDM_FILE_OPEN, L"Открыть файл Орион...");
		}
		CreateKeyboardUI(hwnd, hInst);
		int RIGHT_PANEL_X = 840;
		CreateWindowExW(0, L"BUTTON", L"СБРОС (RESET)", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, RIGHT_PANEL_X, 20, 210, 40, hwnd, (HMENU)2000, hInst, NULL);
		hRegListBox = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL, RIGHT_PANEL_X, 75, 210, 245, hwnd, (HMENU)2001, NULL, NULL);
		orion128_reset(&cpu);
		UpdateRegisterDisplay();
		return 0;
	}
	case WM_SYSCOMMAND: {
		if ((wp & 0xFFF0) == IDM_FILE_OPEN) {
			LoadOrionFile(hwnd);
			return 0;
		}
		return DefWindowProcW(hwnd, msg, wp, lp);
	}
	case WM_COMMAND: {
		int btn_id = LOWORD(wp);

		// 1. Обработка кнопки СБРОС (RESET)
		if (btn_id == 2000) {
			orion128_reset(&cpu);
			for (int i = 0; i < 8; i++) key_matrix_state[i] = 0xFF;
			UpdateRegisterDisplay();
			InvalidateRect(hwnd, NULL, FALSE);
			return 0;
		}

		// 2. Обработка виртуальных клавиш клавиатуры (ID от 1000 до 1071)
		int btn_idx = btn_id - 1000;
		if (btn_idx >= 0 && btn_idx < HARDWARE_MAP_SIZE) {
			UINT target_vk = 0;

			// Находим соответствующий Virtual Key (VK) для этой кнопки
			for (int vk_test = 0; vk_test < 256; vk_test++) {
				int r = -1, c = -1;
				if (MapVirtualKeyToRK(vk_test, r, c)) {
					int hw_code = rk_hardware_map[btn_idx];
					if ((hw_code / 8 == r) && (hw_code % 8 == c)) {
						target_vk = vk_test;
						break;
					}
				}
			}

			// Ручной маппинг системных модификаторов
			if (btn_idx == 57) target_vk = VK_CAPITAL;      // РУС/ЛАТ
			else if (btn_idx == 45) target_vk = VK_SHIFT;   // СС
			else if (btn_idx == 30) target_vk = VK_CONTROL; // УС

			// Если кнопка валидна — зажимаем её в матрице НАПРЯМУЮ
			if (target_vk != 0) {
				// Выделяем 4 кадра удержания (~80 мс), чтобы процессор точно успел считать её
				static_virtual_key_duration = 4;

				if (target_vk == VK_CAPITAL) { rk_rus_lat_pressed = true; }
				else if (target_vk == VK_SHIFT) { rk_cc_pressed = true; }
				else if (target_vk == VK_CONTROL) { rk_us_pressed = true; }
				else {
					int r = -1, c = -1;
					if (MapVirtualKeyToRK(target_vk, r, c)) {
						if (r >= 0 && r < 8) {
							key_matrix_state[r] &= ~(1 << c); // Замыкаем контакт напрямую
						}
					}
				}
			}

			// Возвращаем фокус на главное окно, чтобы продолжала работать физическая клавиатура
			SetFocus(hwnd);
			return 0;
		}
		break;
	}
	case WM_PAINT: {
		PAINTSTRUCT ps; HDC hdc = BeginPaint(hwnd, &ps);
		RenderScreen(hdc, 20, 20); // Рендеринг экрана Ориона
		EndPaint(hwnd, &ps);
		break;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		break;
	default: return DefWindowProcW(hwnd, msg, wp, lp);
	}
	return 0;
}

// Тактов ЦПУ на один звуковой сэмпл (именно частота ЦПУ, а не CLK таймера)
const double TICKS_PER_SAMPLE = SYSTEM_CLOCK_HZ / (double)AUDIO_SAMPLE_RATE;

// Один сэмпл: бипер INTE + меандры каналов ВИ53 (режимы 2 и 3), среднее двух источников.
// Вызывается из потока эмуляции, чтобы out каналов брался в момент выполнения инструкций.
static short MixAudioSample(const CPU8080& c) {
	short sample = c.EI ? 4000 : -4000;
	int active = 0, mixed = 0;
	for (int ch = 0; ch < 3; ch++) {
		const PITChannel& pc = c.pit_channels[ch];
		if (pc.armed && (pc.mode == 2 || pc.mode == 3) && pc.reload > 3) {
			mixed += pc.out ? 1 : -1;
			active++;
		}
	}
	if (active > 0) {
		int vi53 = mixed * 5000 / active;
		sample = (short)((vi53 + sample) / 2);
	}
	return sample;
}

// Аудиопоток только читает готовые сэмплы из кольцевого буфера
void FillAudioBuffer(short* buffer, int samplesCount) {
	for (int i = 0; i < samplesCount; i++) {
		if (inte_ring_read_ptr != inte_ring_write_ptr) {
			buffer[i] = inte_ring_buffer[inte_ring_read_ptr];
			inte_ring_read_ptr = (inte_ring_read_ptr + 1) % INTE_RING_BUF_SIZE;
		}
		else {
			// Если буфер пуст, берем последнее состояние (стабилизация)
			buffer[i] = cpu.EI ? 4000 : -4000;
		}
	}
}

// Флаг завершения: после waveOutReset колбэк получает WOM_DONE для всех буферов и не должен ставить их в очередь снова
static volatile bool audio_closing = false;

void CALLBACK WaveOutCallback(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
	if (uMsg == WOM_DONE && !audio_closing) {
		WAVEHDR* pHdr = (WAVEHDR*)dwParam1;
		FillAudioBuffer((short*)pHdr->lpData, AUDIO_BUF_SIZE);
		waveOutWrite(hwo, pHdr, sizeof(WAVEHDR));
	}
}

static double audio_ticks_accumulator = 0.0;
static uint64_t pit_accum = 0;   // дробный остаток тактов CLK таймера

// =========================================================================
// ИСПРАВЛЕННАЯ СТАБИЛЬНАЯ ФУНКЦИЯ WINMAIN С КАДРОВЫМИ ПРЕРЫВАНИЯМИ 50 ГЦ
// =========================================================================
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
	LoadRoms();
	WNDCLASSW wc = { 0 };
	wc.lpfnWndProc = WndProc;
	wc.hInstance = hInst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"Orion128_Emulator_Core";
	RegisterClassW(&wc);

	HWND hwnd = CreateWindowExW(0, L"Orion128_Emulator_Core", L"Орион-128 Симулятор компьютера",
		WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX,
		CW_USEDEFAULT, CW_USEDEFAULT, 1080, 800, NULL, NULL, hInst, NULL);
	ShowWindow(hwnd, nCmdShow);
	UpdateWindow(hwnd);

	// --- Инициализация звуковой подсистемы waveOut ---
	WAVEFORMATEX wfx = { 0 };
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = 1;
	wfx.nSamplesPerSec = AUDIO_SAMPLE_RATE;
	wfx.wBitsPerSample = 16;
	wfx.nBlockAlign = (wfx.nChannels * wfx.wBitsPerSample) / 8;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

	if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx, (DWORD_PTR)WaveOutCallback, 0, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) {
		for (int i = 0; i < 2; i++) {
			audioBuffers[i] = new short[AUDIO_BUF_SIZE];
			memset(audioBuffers[i], 0, AUDIO_BUF_SIZE * sizeof(short));
			waveHeader[i].lpData = (LPSTR)audioBuffers[i];
			waveHeader[i].dwBufferLength = AUDIO_BUF_SIZE * sizeof(short);
			waveHeader[i].dwFlags = 0;
			waveOutPrepareHeader(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
			waveOutWrite(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
		}
	}

	MSG msg;
	LARGE_INTEGER frequency;
	LARGE_INTEGER last_hardware_time;
	double internal_cycles_debt = 0.0;

	QueryPerformanceFrequency(&frequency);
	QueryPerformanceCounter(&last_hardware_time);

	while (emulator_running) {
		// Ожидание системных событий во избежание 100% загрузки процессора ПК
		MsgWaitForMultipleObjectsEx(0, NULL, 1, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

		while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				emulator_running = false;
				break;
			}

			// --- НАЖАТИЕ КЛАВИШ (WM_KEYDOWN) ---
			if (msg.message == WM_KEYDOWN) {
				if (!(msg.lParam & (1 << 30))) { // Защита от автоповтора Windows
					UINT vk = (UINT)msg.wParam;
					if (vk == VK_CAPITAL) { rk_rus_lat_pressed = true; }
					else if (vk == VK_SHIFT) { rk_cc_pressed = true; }
					else if (vk == VK_CONTROL) { rk_us_pressed = true; }
					else {
						int target_row = -1, target_col = -1;
						if (MapVirtualKeyToRK(vk, target_row, target_col)) {
							key_matrix_state[target_row] &= ~(1 << target_col);
						}
					}
				}
			}

			// --- ОТПУСКАНИЕ КЛАВИШ (WM_KEYUP) ---
			else if (msg.message == WM_KEYUP) {
				UINT vk = (UINT)msg.wParam;
				if (vk == VK_CAPITAL) { rk_rus_lat_pressed = false; }
				else if (vk == VK_SHIFT) { rk_cc_pressed = false; }
				else if (vk == VK_CONTROL) { rk_us_pressed = false; }
				else {
					int target_row = -1, target_col = -1;
					if (MapVirtualKeyToRK(vk, target_row, target_col)) {
						key_matrix_state[target_row] |= (1 << target_col); // Размыкаем контакт
					}
				}
			}

			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		if (!emulator_running) break;

		// --- ДИСПЕТЧЕР И ТАКТОВАЯ СИНХРОНИЗАЦИЯ ЯДРА ---
		LARGE_INTEGER current_hardware_time;
		QueryPerformanceCounter(&current_hardware_time);
		double elapsed_seconds = (double)(current_hardware_time.QuadPart - last_hardware_time.QuadPart) / frequency.QuadPart;

		if (elapsed_seconds > 0.1) elapsed_seconds = 0.1; // Защита от лагов Windows

		if (elapsed_seconds > 0.0) {
			last_hardware_time = current_hardware_time;
			internal_cycles_debt += elapsed_seconds * SYSTEM_CLOCK_HZ; // Накапливаем такты ПК

			int cycles_to_execute = (int)internal_cycles_debt;
			if (cycles_to_execute > 0) {
				while (cycles_to_execute > 0) {
					int elapsed_ticks = cpu8080_step(&cpu);

					cycles_to_execute -= elapsed_ticks;
					internal_cycles_debt -= elapsed_ticks;

					pit_accum += (uint64_t)elapsed_ticks * PIT_CLOCK_HZ;
					if (pit_accum >= SYSTEM_CLOCK_HZ) {
						int ticks = (int)(pit_accum / SYSTEM_CLOCK_HZ);
						pit_accum %= SYSTEM_CLOCK_HZ;
						pit_tick(&cpu, ticks);
					}

					// Накапливаем процессорные такты для генерации звука
					audio_ticks_accumulator += elapsed_ticks;
					while (audio_ticks_accumulator >= TICKS_PER_SAMPLE) {
						audio_ticks_accumulator -= TICKS_PER_SAMPLE;

						// Сэмпл формируется в момент выполнения инструкций: бипер INTE + каналы ВИ53
						short sample = MixAudioSample(cpu);

						// Записываем в кольцевой буфер
						int next_write = (inte_ring_write_ptr + 1) % INTE_RING_BUF_SIZE;
						// Защита от переполнения буфера
						if (next_write != inte_ring_read_ptr) {
							inte_ring_buffer[inte_ring_write_ptr] = sample;
							inte_ring_write_ptr = next_write;
						}
					}

					// Отсчитываем такты до кадрового прерывания 50 Гц
					cpu.cycles_until_interrupt -= elapsed_ticks;
					if (cpu.cycles_until_interrupt <= 0) {
						cpu.cycles_until_interrupt += 50000; // На запуск следующего кадра

						// Автоматическое плавное отжатие экранных кнопок GUI (раз в 20 мс)
						if (static_virtual_key_duration > 0) {
							static_virtual_key_duration--;
							if (static_virtual_key_duration == 0) {
								rk_rus_lat_pressed = false;
								rk_cc_pressed = false;
								rk_us_pressed = false;
								for (int i = 0; i < 8; i++) key_matrix_state[i] = 0xFF;
							}
						}

						// Обновляем графику и окна отладки строго 1 раз по завершении кадра
						UpdateRegisterDisplay();
						InvalidateRect(hwnd, NULL, FALSE);
					}
				}
			}
		}

		if (internal_cycles_debt <= 0.0) {
			Sleep(1); // Освобождаем CPU ПК
		}
	}

	// --- Безопасная очистка аудиодескрипторов перед выходом ---
	if (hWaveOut) {
		audio_closing = true;    // колбэк больше не ставит буферы в очередь
		waveOutReset(hWaveOut); // вернуть все буферы, иначе Unprepare/Close завершатся ошибкой
		for (int i = 0; i < 2; i++) {
			waveOutUnprepareHeader(hWaveOut, &waveHeader[i], sizeof(WAVEHDR));
			delete[] audioBuffers[i];
		}
		waveOutClose(hWaveOut);
	}
	return (int)msg.wParam;
}

