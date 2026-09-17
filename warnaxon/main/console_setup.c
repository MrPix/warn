#include "console_setup.h"

#include "cmd_can.h"
#include "cmd_led.h"
#include "cmd_system.h"
#include "esp_console.h"
#include "esp_err.h"
#include "linenoise/linenoise.h"  // Selects stable vs advanced CLI line editing mode.

void console_start(void)
{
	esp_console_repl_t *repl = NULL;
	esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
	repl_config.prompt = "warnaxon> ";
	repl_config.max_cmdline_length = 128;
	repl_config.max_cmdline_args = 9;
	ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));

	/*
	 * Stable serial terminal mode:
	 * ESP-IDF's console REPL uses linenoise for advanced line editing,
	 * command history, hints, and TAB completion. That mode redraws the
	 * current input line with ANSI cursor-control sequences on every keypress.
	 *
	 * Some PC serial terminals show that redraw as the cursor jumping back to
	 * the start of the line while typing. Disabling multiline + dumb mode keeps
	 * input visually stable, but reduces advanced editing features.
	 *
	 * To switch back to the advanced CLI behavior, remove these two calls:
	 *     linenoiseSetMultiLine(0);
	 *     linenoiseSetDumbMode(1);
	 */
	linenoiseSetMultiLine(0);
	linenoiseSetDumbMode(1);

	ESP_ERROR_CHECK(esp_console_register_help_command());
	register_system_commands();
	register_led_commands();
	register_can_commands();

	ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
