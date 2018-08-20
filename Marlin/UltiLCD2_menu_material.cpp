#define  __STDC_LIMIT_MACROS        // Required to get UINTxx_MAX macros to work in stdint.h (included from avr/pgmspace.h)
#include <avr/pgmspace.h>

#include "Configuration.h"
#ifdef ENABLE_ULTILCD2
#include "Marlin.h"
#include "cardreader.h"//This code uses the card.longFilename as buffer to store data, to save memory.
#include "temperature.h"
#include "UltiLCD2.h"
#include "UltiLCD2_hi_lib.h"
#include "UltiLCD2_menu_material.h"

#ifndef eeprom_read_float
//Arduino IDE compatibility, lacks the eeprom_read_float function
float inline eeprom_read_float(float* addr)
{
    union { uint32_t i; float f; } n;
    n.i = eeprom_read_dword((uint32_t*)addr);
    return n.f;
}
void inline eeprom_write_float(float* addr, float f)
{
    union { uint32_t i; float f; } n;
    n.f = f;
    eeprom_write_dword((uint32_t*)addr, n.i);
}
#endif

struct materialSettings material[EXTRUDERS];
static menuFunc_t post_change_material_menu;
static unsigned long preheat_end_time;
static uint8_t nozzle_select_index;
static bool material_load_successful;

static void lcd_menu_material_main();
static void lcd_menu_change_material_preheat();
static void lcd_menu_change_material_remove();
static void lcd_menu_change_material_remove_wait_user();
static void lcd_menu_change_material_remove_wait_user_ready();
static void lcd_menu_change_material_select_material();
static void lcd_menu_insert_material_preheat();
static void lcd_menu_change_material_insert_wait_user();
static void lcd_menu_change_material_insert_wait_user_ready();
static void lcd_menu_change_material_insert_forward();
static void lcd_menu_change_material_insert();
static void lcd_menu_material_select();
static void lcd_menu_material_selected();
static void lcd_menu_material_settings();
static void lcd_menu_material_temperature_settings();
static void lcd_menu_material_retraction_settings();
static void lcd_menu_material_retraction_settings_per_nozzle();
static void lcd_menu_material_settings_store();
static bool hasInvalidNozzleTemperature(uint16_t temperature);
static bool hasInvalidBedTemperature(uint16_t temperature);
static bool hasInvalidFanSpeed(uint8_t fanspeed);
static bool hasInvalidMaterialFlow(uint16_t flow);
static bool hasInvalidDiameter(float diameter);
static bool hasInvalidRetractionLength(uint16_t length);
static bool hasInvalidRetractionSpeed(uint16_t speed);
static uint8_t strToUint8(char* str);
static uint16_t strToUint16(char* str, uint8_t scaling_factor=1);

static void cancelMaterialInsert()
{
    set_extrude_min_temp(EXTRUDE_MINTEMP);
    digipot_current(2, motor_current_setting[2]);//Set E motor power to default.
}

void lcd_menu_material()
{
#if EXTRUDERS > 1
    lcd_tripple_menu(PSTR("PRIMARY|NOZZLE"), PSTR("SECONDARY|NOZZLE"), PSTR("RETURN"));

    if (lcd_lib_button_pressed)
    {
        if (IS_SELECTED_MAIN(0))
        {
            active_extruder = 0;
            lcd_change_to_menu(lcd_menu_material_main);
        }
        else if (IS_SELECTED_MAIN(1))
        {
            active_extruder = 1;
            lcd_change_to_menu(lcd_menu_material_main);
        }
        else if (IS_SELECTED_MAIN(2))
            lcd_change_to_menu(lcd_menu_main);
    }

    lcd_lib_update_screen();
#else
    currentMenu = lcd_menu_material_main;
#endif
}

static void lcd_menu_material_main_return()
{
    doCooldown();
    enquecommand_P(PSTR("G28 X0 Y0"));
    currentMenu = lcd_menu_material_main;
}

static void lcd_menu_material_main()
{
    lcd_tripple_menu(PSTR("CHANGE"), PSTR("SETTINGS"), PSTR("RETURN"));

    if (lcd_lib_button_pressed)
    {
        if (IS_SELECTED_MAIN(0) && !is_command_queued())
        {
            minProgress = 0;
            char buffer[32];
            enquecommand_P(PSTR("G28 X0 Y0"));
            sprintf_P(buffer, PSTR("G1 F%i X%i Y%i"), int(homing_feedrate[0]), int(X_MAX_LENGTH/2), 10);
            enquecommand(buffer);
            lcd_change_to_menu_change_material(lcd_menu_material_main_return);
        }
        else if (IS_SELECTED_MAIN(1))
            lcd_change_to_menu(lcd_menu_material_select, SCROLL_MENU_ITEM_POS(0));
        else if (IS_SELECTED_MAIN(2))
            lcd_change_to_menu(lcd_menu_main);
    }

    lcd_lib_update_screen();
}

void lcd_change_to_menu_change_material(menuFunc_t return_menu)
{
    post_change_material_menu = return_menu;
    preheat_end_time = millis() + (unsigned long)material[active_extruder].change_preheat_wait_time * 1000L;
    lcd_change_to_menu(lcd_menu_change_material_preheat);
}

static void lcd_menu_change_material_preheat()
{
#ifdef USE_CHANGE_TEMPERATURE
    setTargetHotend(material[active_extruder].change_temperature, active_extruder);
#else
    setTargetHotend(material[active_extruder].temperature[0], active_extruder);
#endif
    int16_t temp = degHotend(active_extruder) - 20;
    int16_t target = degTargetHotend(active_extruder) - 20;
    if (temp < 0) temp = 0;
    if (temp > target - 5 && temp < target + 5)
    {
        // Besides the nozzle heating up, we want a minimum time for the material to get hot as well (not only on the outside).
        if (((signed long)(millis() - preheat_end_time) > 0)
            || card.pause)      // Optimization: No need to wait when we are paused as the material is molten already.
        {
            set_extrude_min_temp(0);

            // Do a forward push before pulling back the material, reducing blobs at the end of the filament.
            plan_set_e_position(0);
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], 20.0 / volume_to_filament_length[active_extruder], retract_feedrate/60.0, active_extruder);

            float old_max_feedrate_e = max_feedrate[E_AXIS];
            float old_retract_acceleration = retract_acceleration;
            float old_max_e_jerk = max_e_jerk;
            max_feedrate[E_AXIS] = FILAMENT_REVERSAL_SPEED;
            retract_acceleration = FILAMENT_LONG_MOVE_ACCELERATION;
            max_e_jerk = FILAMENT_LONG_MOVE_JERK;

            plan_set_e_position(0);
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], -1.0 / volume_to_filament_length[active_extruder], FILAMENT_REVERSAL_SPEED, active_extruder);
            plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], -FILAMENT_REVERSAL_LENGTH / volume_to_filament_length[active_extruder], FILAMENT_REVERSAL_SPEED, active_extruder);

            max_feedrate[E_AXIS] = old_max_feedrate_e;
            retract_acceleration = old_retract_acceleration;
            max_e_jerk = old_max_e_jerk;

            currentMenu = lcd_menu_change_material_remove;
            temp = target;
        }
    }
    else
    {
#ifdef USE_CHANGE_TEMPERATURE
        preheat_end_time = millis() + (unsigned long)material[active_extruder].change_preheat_wait_time * 1000L;
#else
        preheat_end_time = millis();
#endif
    }

    uint8_t progress = uint8_t(temp * 125 / target);
    if (progress < minProgress)
        progress = minProgress;
    else
        minProgress = progress;

    lcd_info_screen(post_change_material_menu, cancelMaterialInsert);
    lcd_lib_draw_stringP(3, 10, PSTR("Heating printhead"));
    lcd_lib_draw_stringP(3, 20, PSTR("for material removal"));

    lcd_progressbar(progress);

    lcd_lib_update_screen();
}

static void lcd_menu_change_material_remove()
{
    lcd_info_screen(post_change_material_menu, cancelMaterialInsert);
    lcd_lib_draw_stringP(3, 20, PSTR("Reversing material"));

    if (!blocks_queued())
    {
        lcd_lib_beep();
        led_glow_dir = led_glow = 0;
        currentMenu = lcd_menu_change_material_remove_wait_user;
        SELECT_MAIN_MENU_ITEM(0);
        //Disable the extruder motor so you can pull out the remaining filament.
        disable_e0();
        disable_e1();
        disable_e2();
    }

    long pos = -st_get_position(E_AXIS);
    long targetPos = lround(FILAMENT_REVERSAL_LENGTH * axis_steps_per_unit[E_AXIS]);
    uint8_t progress = (pos * 125 / targetPos);
    lcd_progressbar(progress);

    lcd_lib_update_screen();
}

static void lcd_menu_change_material_remove_wait_user_ready()
{
    plan_set_e_position(0);
    lcd_change_to_menu(lcd_menu_change_material_select_material);
}

static void lcd_menu_change_material_remove_wait_user()
{
    LED_GLOW();
    setTargetHotend(material[active_extruder].temperature[0], active_extruder);

    lcd_question_screen(NULL, lcd_menu_change_material_remove_wait_user_ready, PSTR("READY"), post_change_material_menu, cancelMaterialInsert, PSTR("CANCEL"));
    lcd_lib_draw_string_centerP(20, PSTR("Remove material"));
    lcd_lib_update_screen();
}

static char* lcd_menu_change_material_select_material_callback(uint8_t nr)
{
    eeprom_read_block(card.longFilename, EEPROM_MATERIAL_NAME_OFFSET(nr), MATERIAL_NAME_SIZE);
    card.longFilename[MATERIAL_NAME_SIZE] = '\0';
    return card.longFilename;
}

static void lcd_menu_change_material_select_material_details_callback(uint8_t nr)
{
    char buffer[32];
    char* c = buffer;

    // Update meta data; a timer based toggle between two sets of text to show.
    if (led_glow_dir)
    {
        c = float_to_string(eeprom_read_float(EEPROM_MATERIAL_DIAMETER_OFFSET(nr)), c, PSTR("mm"));
        while(c < buffer + 10) *c++ = ' ';
        strcpy_P(c, PSTR("Flow:"));
        c += 5;
        c = int_to_string(eeprom_read_word(EEPROM_MATERIAL_FLOW_OFFSET(nr)), c, PSTR("%"));
    }else{
        c = int_to_string(eeprom_read_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(nr, 0)), c, PSTR("C"));
#if TEMP_SENSOR_BED != 0
        *c++ = ' ';
        c = int_to_string(eeprom_read_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(nr)), c, PSTR("C"));
#endif
        while(c < buffer + 10) *c++ = ' ';
        strcpy_P(c, PSTR("Fan: "));
        c += 5;
        c = int_to_string(eeprom_read_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(nr)), c, PSTR("%"));
    }
    lcd_lib_draw_string(5, 53, buffer);
}

static void lcd_menu_change_material_select_material()
{
    uint8_t count = eeprom_read_byte(EEPROM_MATERIAL_COUNT_OFFSET());

    lcd_scroll_menu(PSTR("MATERIAL"), count, lcd_menu_change_material_select_material_callback, lcd_menu_change_material_select_material_details_callback);
    if (lcd_lib_button_pressed)
    {
        lcd_material_set_material(SELECTED_SCROLL_MENU_ITEM(), active_extruder);

        lcd_change_to_menu(lcd_menu_insert_material_preheat, MAIN_MENU_ITEM_POS(0));
    }
}

void lcd_change_to_menu_insert_material(menuFunc_t return_menu)
{
    post_change_material_menu = return_menu;
    currentMenu = lcd_menu_insert_material_preheat;
}

static void lcd_menu_insert_material_preheat()
{
    setTargetHotend(material[active_extruder].temperature[0], active_extruder);
    int16_t temp = degHotend(active_extruder) - 20;
    int16_t target = degTargetHotend(active_extruder) - 20 - 10;
    if (temp < 0) temp = 0;
    if (temp > target && temp < target + 20 && (card.pause || !is_command_queued()))
    {
        set_extrude_min_temp(0);
        currentMenu = lcd_menu_change_material_insert_wait_user;
        temp = target;
    }

    uint8_t progress = uint8_t(temp * 125 / target);
    if (progress < minProgress)
        progress = minProgress;
    else
        minProgress = progress;

    lcd_info_screen(post_change_material_menu, cancelMaterialInsert);
    if (temp < target + 10)
        lcd_lib_draw_stringP(3, 10, PSTR("Heating printhead for"));
    else
        lcd_lib_draw_stringP(3, 10, PSTR("Cooling printhead for"));
    lcd_lib_draw_stringP(3, 20, PSTR("material insertion"));

    lcd_progressbar(progress);

    lcd_lib_update_screen();
}

static void lcd_menu_change_material_insert_wait_user()
{
    LED_GLOW();

    if (printing_state == PRINT_STATE_NORMAL && movesplanned() < 2)
    {
        plan_set_e_position(0);
        plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], 0.5 / volume_to_filament_length[active_extruder], FILAMENT_INSERT_SPEED, active_extruder);
    }

    lcd_question_screen(NULL, lcd_menu_change_material_insert_wait_user_ready, PSTR("READY"), post_change_material_menu, cancelMaterialInsert, PSTR("CANCEL"));
    lcd_lib_draw_string_centerP(10, PSTR("Insert new material"));
    lcd_lib_draw_string_centerP(20, PSTR("from the backside of"));
    lcd_lib_draw_string_centerP(30, PSTR("your machine,"));
    lcd_lib_draw_string_centerP(40, PSTR("above the arrow."));
    lcd_lib_update_screen();
}

static void lcd_menu_change_material_insert_wait_user_ready()
{
    //Override the max feedrate and acceleration values to get a better insert speed and speedup/slowdown
    float old_max_feedrate_e = max_feedrate[E_AXIS];
    float old_retract_acceleration = retract_acceleration;
    float old_max_e_jerk = max_e_jerk;
    max_feedrate[E_AXIS] = FILAMENT_INSERT_FAST_SPEED;
    retract_acceleration = FILAMENT_LONG_MOVE_ACCELERATION;
    max_e_jerk = FILAMENT_LONG_MOVE_JERK;

    plan_set_e_position(0);
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], FILAMENT_FORWARD_LENGTH / volume_to_filament_length[active_extruder], FILAMENT_INSERT_FAST_SPEED, active_extruder);

    //Put back origonal values.
    max_feedrate[E_AXIS] = old_max_feedrate_e;
    retract_acceleration = old_retract_acceleration;
    max_e_jerk = old_max_e_jerk;

    lcd_change_to_menu(lcd_menu_change_material_insert_forward);
}

static void lcd_menu_change_material_insert_forward()
{
    lcd_info_screen(post_change_material_menu, cancelMaterialInsert);
    lcd_lib_draw_stringP(3, 20, PSTR("Forwarding material"));

    if (!blocks_queued())
    {
        lcd_lib_beep();
        led_glow_dir = led_glow = 0;

        digipot_current(2, motor_current_setting[2]*2/3);//Set the E motor power lower so we skip instead of grind.
        currentMenu = lcd_menu_change_material_insert;
        SELECT_MAIN_MENU_ITEM(0);
    }

    long pos = st_get_position(E_AXIS);
    long targetPos = lround(FILAMENT_FORWARD_LENGTH*axis_steps_per_unit[E_AXIS]);
    uint8_t progress = (pos * 125 / targetPos);
    lcd_progressbar(progress);

    lcd_lib_update_screen();
}

static void materialInsertReady()
{
    plan_set_e_position(0);
    plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], -END_OF_PRINT_RETRACTION / volume_to_filament_length[active_extruder], 25*60, active_extruder);
    cancelMaterialInsert();
}

static void lcd_menu_change_material_insert()
{
    LED_GLOW();

    lcd_question_screen(post_change_material_menu, materialInsertReady, PSTR("READY"), post_change_material_menu, cancelMaterialInsert, PSTR("CANCEL"));
    lcd_lib_draw_string_centerP(20, PSTR("Wait till material"));
    lcd_lib_draw_string_centerP(30, PSTR("comes out the nozzle"));

    if (movesplanned() < 2)
    {
        plan_set_e_position(0);
        plan_buffer_line(current_position[X_AXIS], current_position[Y_AXIS], current_position[Z_AXIS], 0.5 / volume_to_filament_length[active_extruder], FILAMENT_INSERT_EXTRUDE_SPEED, active_extruder);
    }

    lcd_lib_update_screen();
}

static void lcd_menu_material_export_done()
{
    lcd_lib_encoder_pos = MAIN_MENU_ITEM_POS(0);
    lcd_info_screen(lcd_menu_material_select, NULL, PSTR("OK"));
    lcd_lib_draw_string_centerP(20, PSTR("Saved materials"));
    lcd_lib_draw_string_centerP(30, PSTR("to the SD card"));
    lcd_lib_draw_string_centerP(40, PSTR("in MATERIAL.TXT"));
    lcd_lib_update_screen();
}

static void lcd_menu_material_export()
{
    if (!card.sdInserted)
    {
        LED_GLOW();
        lcd_lib_encoder_pos = MAIN_MENU_ITEM_POS(0);
        lcd_info_screen(lcd_menu_material_select);
        lcd_lib_draw_string_centerP(15, PSTR("No SD-CARD!"));
        lcd_lib_draw_string_centerP(25, PSTR("Please insert card"));
        lcd_lib_update_screen();
        card.release();
        return;
    }
    if (!card.isOk())
    {
        lcd_info_screen(lcd_menu_material_select);
        lcd_lib_draw_string_centerP(16, PSTR("Reading card..."));
        lcd_lib_update_screen();
        card.initsd();
        return;
    }

    card.setroot();
    card.openFile("MATERIAL.TXT", false);
    uint8_t count = eeprom_read_byte(EEPROM_MATERIAL_COUNT_OFFSET());
    for(uint8_t n=0; n<count; n++)
    {
        char buffer[32];
        strcpy_P(buffer, PSTR("[material]\n"));
        card.write_string(buffer);

        strcpy_P(buffer, PSTR("name="));
        char* ptr = buffer + strlen(buffer);
        eeprom_read_block(ptr, EEPROM_MATERIAL_NAME_OFFSET(n), MATERIAL_NAME_SIZE);
        ptr[MATERIAL_NAME_SIZE] = '\0';
        strcat_P(buffer, PSTR("\n"));
        card.write_string(buffer);

        strcpy_P(buffer, PSTR("temperature="));
        ptr = buffer + strlen(buffer);
        int_to_string(eeprom_read_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(n)), ptr, PSTR("\n"));
        card.write_string(buffer);

        for(uint8_t nozzle=0; nozzle<MATERIAL_NOZZLE_COUNT; nozzle++)
        {
            strcpy_P(buffer, PSTR("temperature_"));
            ptr = buffer + strlen(buffer);
            ptr = float_to_string(nozzleIndexToNozzleSize(nozzle), ptr, PSTR("="));
            int_to_string(eeprom_read_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(n, nozzle)), ptr, PSTR("\n"));
            card.write_string(buffer);

            strcpy_P(buffer, PSTR("retraction_length_"));
            ptr = buffer + strlen(buffer);
            ptr = float_to_string(nozzleIndexToNozzleSize(nozzle), ptr, PSTR("="));
            float_to_string(float(eeprom_read_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(n, nozzle))) / EEPROM_RETRACTION_LENGTH_SCALE, ptr, PSTR("\n"));
            card.write_string(buffer);

            strcpy_P(buffer, PSTR("retraction_speed_"));
            ptr = buffer + strlen(buffer);
            ptr = float_to_string(nozzleIndexToNozzleSize(nozzle), ptr, PSTR("="));
            float_to_string(float(eeprom_read_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(n, nozzle))) / EEPROM_RETRACTION_SPEED_SCALE, ptr, PSTR("\n"));
            card.write_string(buffer);
        }

#if TEMP_SENSOR_BED != 0
        strcpy_P(buffer, PSTR("bed_temperature="));
        ptr = buffer + strlen(buffer);
        int_to_string(eeprom_read_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(n)), ptr, PSTR("\n"));
        card.write_string(buffer);
#endif

        strcpy_P(buffer, PSTR("fan_speed="));
        ptr = buffer + strlen(buffer);
        int_to_string(eeprom_read_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(n)), ptr, PSTR("\n"));
        card.write_string(buffer);

        strcpy_P(buffer, PSTR("flow="));
        ptr = buffer + strlen(buffer);
        int_to_string(eeprom_read_word(EEPROM_MATERIAL_FLOW_OFFSET(n)), ptr, PSTR("\n"));
        card.write_string(buffer);

        strcpy_P(buffer, PSTR("diameter="));
        ptr = buffer + strlen(buffer);
        float_to_string(eeprom_read_float(EEPROM_MATERIAL_DIAMETER_OFFSET(n)), ptr, PSTR("\n"));
        card.write_string(buffer);

#ifdef USE_CHANGE_TEMPERATURE
        strcpy_P(buffer, PSTR("change_temp="));
        ptr = buffer + strlen(buffer);
        float_to_string(eeprom_read_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(n)), ptr, PSTR("\n"));
        card.write_string(buffer);

        strcpy_P(buffer, PSTR("change_wait="));
        ptr = buffer + strlen(buffer);
        float_to_string(eeprom_read_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(n)), ptr, PSTR("\n\n"));
        card.write_string(buffer);
#endif
    }
    card.closefile();

    currentMenu = lcd_menu_material_export_done;
}

static void lcd_menu_material_import_done()
{
    lcd_lib_encoder_pos = MAIN_MENU_ITEM_POS(0);
    lcd_info_screen(lcd_menu_material_select, NULL, PSTR("OK"));
    if(material_load_successful)
    {
        lcd_lib_draw_string_centerP(20, PSTR("Loaded materials"));
        lcd_lib_draw_string_centerP(30, PSTR("from the SD card"));
    }
    else
    {
        lcd_lib_draw_string_centerP(10, PSTR("Error during"));
        lcd_lib_draw_string_centerP(20, PSTR("material load"));
        lcd_lib_draw_string_centerP(30, PSTR("from the SD card"));
    }
    lcd_lib_update_screen();
}

static uint8_t strToUint8(char* str)
{
    return min(strToUint16(str), UINT8_MAX);
}

// Extracts a value from the given string and applies a scaling factor before converting the result to an uint16_t.
// Advantage of using this function is that it handles underflows and overflows in a controlled manner by limiting the
// returned values to what is allowed in a uint16, i.e. it returns a 0 or UINT16_MAX.
// @param str is a C-string beginning with the representation of a floating-point number.
// @param scaling_factor is an optional scaling factor to allow for more precision when storing the data in EEPROM as a scaled integer.
// @return the converted value as an uint16. On underflow limited to a 0, or on overflow limited to UINT16_MAX.
static uint16_t strToUint16(char* str, uint8_t scaling_factor)
{
    double value = strtod(str, NULL);
    value *= scaling_factor;
    return max(0, min(value, UINT16_MAX));
}

static void lcd_menu_material_import()
{
    if (!card.sdInserted)
    {
        LED_GLOW();
        lcd_lib_encoder_pos = MAIN_MENU_ITEM_POS(0);
        lcd_info_screen(lcd_menu_material_select);
        lcd_lib_draw_string_centerP(15, PSTR("No SD-CARD!"));
        lcd_lib_draw_string_centerP(25, PSTR("Please insert card"));
        lcd_lib_update_screen();
        card.release();
        return;
    }
    if (!card.isOk())
    {
        lcd_info_screen(lcd_menu_material_select);
        lcd_lib_draw_string_centerP(16, PSTR("Reading card..."));
        lcd_lib_update_screen();
        card.initsd();
        return;
    }

    card.setroot();
    card.openFile("MATERIAL.TXT", true);
    if (!card.isFileOpen())
    {
        lcd_lib_encoder_pos = MAIN_MENU_ITEM_POS(0);
        lcd_info_screen(lcd_menu_material_select);
        lcd_lib_draw_string_centerP(15, PSTR("No material file"));
        lcd_lib_draw_string_centerP(25, PSTR("found on card."));
        lcd_lib_update_screen();
        return;
    }

    SERIAL_ECHO_START;
    SERIAL_ECHOLNPGM("Start read materials");
    material_load_successful = true;  // Set to false during error handling

    char buffer[32];
    uint8_t count = uint8_t(-1);
    while(card.fgets(buffer, sizeof(buffer)) > 0)
    {
        buffer[sizeof(buffer)-1] = '\0';
        char* c = strchr(buffer, '\n');
        if (c) *c = '\0';

        if(strcmp_P(buffer, PSTR("[material]")) == 0)
        {
            count++;
        }else if (count < EEPROM_MATERIAL_SETTINGS_MAX_COUNT)
        {
            c = strchr(buffer, '=');
            if (c)
            {
                *c++ = '\0';
                if (strcmp_P(buffer, PSTR("name")) == 0)
                {
                    eeprom_write_block(c, EEPROM_MATERIAL_NAME_OFFSET(count), MATERIAL_NAME_SIZE);
                    SERIAL_ECHO_START;
                    SERIAL_ECHOPGM("Adding material ");
                    SERIAL_ECHOLN(c);
                }else if (strcmp_P(buffer, PSTR("temperature")) == 0)
                {
                    uint16_t temperature = strToUint16(c);
                    if (hasInvalidNozzleTemperature(temperature)) {
                        temperature = 210;  // Default copied from PLA
                        SERIAL_ERROR_START;
                        SERIAL_ERRORLNPGM("hasInvalidNozzleTemperature found problem");
                        material_load_successful = false;
                    }
                    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(count), temperature);
                }else if (strcmp_P(buffer, PSTR("bed_temperature")) == 0) {
                    uint16_t bed_temperature = strToUint16(c);
                    if (hasInvalidBedTemperature(bed_temperature))
                    {
                        bed_temperature = 60;  // Default copied from PLA
                        SERIAL_ERROR_START;
                        SERIAL_ERRORLNPGM("hasInvalidBedTemperature found problem");
                        material_load_successful = false;
                    }
                    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(count), bed_temperature);
                }else if (strcmp_P(buffer, PSTR("fan_speed")) == 0)
                {
                    uint8_t fan_speed = strToUint8(c);
                    if (hasInvalidFanSpeed(fan_speed)) {
                        fan_speed = 100; // Default copied from PLA
                        SERIAL_ERROR_START;
                        SERIAL_ERRORLNPGM("hasInvalidFanSpeed found problem");
                        material_load_successful = false;
                    }
                    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(count), fan_speed);
                }else if (strcmp_P(buffer, PSTR("flow")) == 0)
                {
                    uint16_t flow = strToUint16(c);
                    if (hasInvalidMaterialFlow(flow)) {
                        flow = 100; // Default copied from PLA
                        SERIAL_ERROR_START;
                        SERIAL_ERRORLNPGM("hasInvalidMaterialFlow found problem");
                        material_load_successful = false;
                    }
                    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(count), flow);
                }else if (strcmp_P(buffer, PSTR("diameter")) == 0)
                {
                    double diameter = strtod(c, NULL);
                    if (hasInvalidDiameter(diameter)) {
                        diameter = 2.85; // Default copied from PLA
                        SERIAL_ERROR_START;
                        SERIAL_ERRORLNPGM("hasInvalidDiameter found problem");
                        material_load_successful = false;
                    }
                    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(count), diameter);
#ifdef USE_CHANGE_TEMPERATURE
                }else if (strcmp_P(buffer, PSTR("change_temp")) == 0)
                {
                    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(count), strtol(c, NULL, 10));
                }else if (strcmp_P(buffer, PSTR("change_wait")) == 0)
                {
                    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(count), strtol(c, NULL, 10));
#endif
                }
                for(uint8_t nozzle=0; nozzle<MATERIAL_NOZZLE_COUNT; nozzle++)
                {
                    char buffer2[32];
                    strcpy_P(buffer2, PSTR("temperature_"));
                    char* ptr = buffer2 + strlen(buffer2);
                    float_to_string(nozzleIndexToNozzleSize(nozzle), ptr);
                    if (strcmp(buffer, buffer2) == 0)
                    {
                        uint16_t extra_temperature = strToUint16(c);
                        if (hasInvalidNozzleTemperature(extra_temperature)) {
                            extra_temperature = 210; // Default copied from PLA
                            SERIAL_ERROR_START;
                            SERIAL_ERRORLNPGM("hasInvalidNozzleTemperature found problem");
                            material_load_successful = false;
                        }
                        eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(count, nozzle), extra_temperature);
                    }

                    strcpy_P(buffer2, PSTR("retraction_length_"));
                    ptr = buffer2 + strlen(buffer2);
                    ptr = float_to_string(nozzleIndexToNozzleSize(nozzle), ptr);
                    if (strcmp(buffer, buffer2) == 0)
                    {
                        uint16_t retraction_length = strToUint16(c, EEPROM_RETRACTION_LENGTH_SCALE);
                        if (hasInvalidRetractionLength(retraction_length)) {
                            retraction_length = 6.5 * EEPROM_RETRACTION_LENGTH_SCALE;   // Default copied from PLA
                            SERIAL_ERROR_START;
                            SERIAL_ERRORLNPGM("hasInvalidRetractionLength found problem");
                            material_load_successful = false;
                        }
                        eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(count, nozzle), retraction_length);
                    }

                    strcpy_P(buffer2, PSTR("retraction_speed_"));
                    ptr = buffer2 + strlen(buffer2);
                    ptr = float_to_string(nozzleIndexToNozzleSize(nozzle), ptr);
                    if (strcmp(buffer, buffer2) == 0)
                    {
                        uint16_t retraction_speed = strToUint16(c, EEPROM_RETRACTION_SPEED_SCALE);
                        if (hasInvalidRetractionSpeed(retraction_speed)) {
                            retraction_speed = 25 * EEPROM_RETRACTION_SPEED_SCALE;  // Default copied from PLA
                            SERIAL_ERROR_START;
                            SERIAL_ERRORLNPGM("hasInvalidRetractionSpeed found problem");
                            material_load_successful = false;
                        }
                        eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(count, nozzle), retraction_speed);
                    }
                }
            }
        }
    }
    count++;
    if (count > 0)
    {
        eeprom_write_byte(EEPROM_MATERIAL_COUNT_OFFSET(), count);
    }
    card.closefile();

    currentMenu = lcd_menu_material_import_done;
}

static char* lcd_material_select_callback(uint8_t nr)
{
    uint8_t count = eeprom_read_byte(EEPROM_MATERIAL_COUNT_OFFSET());
    if (nr == 0)
        strcpy_P(card.longFilename, PSTR("< RETURN"));
    else if (nr == count + 1)
        strcpy_P(card.longFilename, PSTR("Customize"));
    else if (nr == count + 2)
        strcpy_P(card.longFilename, PSTR("Export to SD"));
    else if (nr == count + 3)
        strcpy_P(card.longFilename, PSTR("Import from SD"));
    else{
        eeprom_read_block(card.longFilename, EEPROM_MATERIAL_NAME_OFFSET(nr - 1), MATERIAL_NAME_SIZE);
        card.longFilename[MATERIAL_NAME_SIZE] = '\0';
    }
    return card.longFilename;
}

static void lcd_material_select_details_callback(uint8_t nr)
{
    uint8_t count = eeprom_read_byte(EEPROM_MATERIAL_COUNT_OFFSET());
    if (nr == 0)
    {

    }
    else if (nr <= count)
    {
        char buffer[32];
        char* c = buffer;
        nr -= 1;

        // Update meta data; a timer based toggle between two sets of text to show.
        if (led_glow_dir)
        {
            c = float_to_string(eeprom_read_float(EEPROM_MATERIAL_DIAMETER_OFFSET(nr)), c, PSTR("mm"));
            while(c < buffer + 10) *c++ = ' ';
            strcpy_P(c, PSTR("Flow:"));
            c += 5;
            c = int_to_string(eeprom_read_word(EEPROM_MATERIAL_FLOW_OFFSET(nr)), c, PSTR("%"));
        }else{
            c = int_to_string(eeprom_read_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(nr, 0)), c, PSTR("C"));
#if TEMP_SENSOR_BED != 0
            *c++ = ' ';
            c = int_to_string(eeprom_read_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(nr)), c, PSTR("C"));
#endif
            while(c < buffer + 10) *c++ = ' ';
            strcpy_P(c, PSTR("Fan: "));
            c += 5;
            c = int_to_string(eeprom_read_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(nr)), c, PSTR("%"));
        }
        lcd_lib_draw_string(5, 53, buffer);
    }else if (nr == count + 1)
    {
        lcd_lib_draw_string_centerP(53, PSTR("Modify the settings"));
    }else if (nr == count + 2)
    {
        lcd_lib_draw_string_centerP(53, PSTR("Saves all materials"));
    }else if (nr == count + 3)
    {
        lcd_lib_draw_string_centerP(53, PSTR("Loads all materials"));
    }
}

static void lcd_menu_material_select()
{
    uint8_t count = eeprom_read_byte(EEPROM_MATERIAL_COUNT_OFFSET());

    lcd_scroll_menu(PSTR("MATERIAL"), count + 4, lcd_material_select_callback, lcd_material_select_details_callback);
    if (lcd_lib_button_pressed)
    {
        if (IS_SELECTED_SCROLL(0))
            lcd_change_to_menu(lcd_menu_material_main);
        else if (IS_SELECTED_SCROLL(count + 1))
            lcd_change_to_menu(lcd_menu_material_settings);
        else if (IS_SELECTED_SCROLL(count + 2))
            lcd_change_to_menu(lcd_menu_material_export);
        else if (IS_SELECTED_SCROLL(count + 3))
            lcd_change_to_menu(lcd_menu_material_import);
        else{
            lcd_material_set_material(SELECTED_SCROLL_MENU_ITEM() - 1, active_extruder);

            post_change_material_menu = lcd_menu_material_main;
            lcd_change_to_menu(lcd_menu_material_selected, MAIN_MENU_ITEM_POS(0));
        }
    }
}

static void lcd_menu_material_selected()
{
    lcd_info_screen(post_change_material_menu, NULL, PSTR("OK"));
    lcd_lib_draw_string_centerP(20, PSTR("Selected material:"));
    lcd_lib_draw_string_center(30, card.longFilename);
#if EXTRUDERS > 1
    if (active_extruder == 0)
        lcd_lib_draw_string_centerP(40, PSTR("for primary nozzle"));
    else if (active_extruder == 1)
        lcd_lib_draw_string_centerP(40, PSTR("for secondary nozzle"));
#endif
    lcd_lib_update_screen();
}

static char* lcd_material_settings_callback(uint8_t nr)
{
    if (nr == 0)
        strcpy_P(card.longFilename, PSTR("< RETURN"));
    else if (nr == 1)
        strcpy_P(card.longFilename, PSTR("Temperature"));
#if TEMP_SENSOR_BED != 0
    else if (nr == 2)
        strcpy_P(card.longFilename, PSTR("Heated buildplate"));
#endif
    else if (nr == 2 + BED_MENU_OFFSET)
        strcpy_P(card.longFilename, PSTR("Diameter"));
    else if (nr == 3 + BED_MENU_OFFSET)
        strcpy_P(card.longFilename, PSTR("Fan"));
    else if (nr == 4 + BED_MENU_OFFSET)
        strcpy_P(card.longFilename, PSTR("Flow %"));
#ifdef USE_CHANGE_TEMPERATURE
    else if (nr == 5 + BED_MENU_OFFSET)
        strcpy_P(card.longFilename, PSTR("Change temperature"));
    else if (nr == 6 + BED_MENU_OFFSET)
        strcpy_P(card.longFilename, PSTR("Change wait time"));
#endif
    else if (nr == 5 + USE_CHANGE_TEMPERATURE_MENU_OFFSET + BED_MENU_OFFSET)
        strcpy_P(card.longFilename, PSTR("Retraction"));
    else if (nr == 6 + USE_CHANGE_TEMPERATURE_MENU_OFFSET + BED_MENU_OFFSET)
        strcpy_P(card.longFilename, PSTR("Store as preset"));
    else
        strcpy_P(card.longFilename, PSTR("???"));
    return card.longFilename;
}

static void lcd_material_settings_details_callback(uint8_t nr)
{
    char buffer[20];
    buffer[0] = '\0';
    if (nr == 0)
    {
        strlcpy(buffer, material[active_extruder].name, sizeof(buffer));
    }else if (nr == 1)
    {
        // Update meta data; a timer based toggle between two sets of text to show.
        if (!led_glow_dir)  // Start showing the first nozzle temperatures.
        {
            char* c = buffer;
            for(uint8_t n=0; n<3; n++)
                c = int_to_string(material[active_extruder].temperature[n], c, PSTR("C "));
        }else{
            char* c = buffer;
            for(uint8_t n=3; n<MATERIAL_NOZZLE_COUNT; n++)
                c = int_to_string(material[active_extruder].temperature[n], c, PSTR("C "));
        }
#if TEMP_SENSOR_BED != 0
    }else if (nr == 2)
    {
        int_to_string(material[active_extruder].bed_temperature, buffer, PSTR("C"));
#endif
    }else if (nr == 2 + BED_MENU_OFFSET)
    {
        float_to_string(material[active_extruder].diameter, buffer, PSTR("mm"));
    }else if (nr == 3 + BED_MENU_OFFSET)
    {
        int_to_string(material[active_extruder].fan_speed, buffer, PSTR("%"));
    }else if (nr == 4 + BED_MENU_OFFSET)
    {
        int_to_string(material[active_extruder].flow, buffer, PSTR("%"));
#ifdef USE_CHANGE_TEMPERATURE
    }else if (nr == 5 + BED_MENU_OFFSET)
    {
        int_to_string(material[active_extruder].change_temperature, buffer, PSTR("C"));
    }else if (nr == 6 + BED_MENU_OFFSET)
    {
        int_to_string(material[active_extruder].change_preheat_wait_time, buffer, PSTR("Sec"));
#endif
    }
    lcd_lib_draw_string(5, 53, buffer);
}

static void lcd_menu_material_settings()
{
    lcd_scroll_menu(PSTR("MATERIAL"), 7 + USE_CHANGE_TEMPERATURE_MENU_OFFSET + BED_MENU_OFFSET, lcd_material_settings_callback, lcd_material_settings_details_callback);
    if (lcd_lib_button_pressed)
    {
        if (IS_SELECTED_SCROLL(0))
        {
            lcd_change_to_menu(lcd_menu_material_main);
            lcd_material_store_current_material();
        }else if (IS_SELECTED_SCROLL(1))
        {
            //LCD_EDIT_SETTING(material[active_extruder].temperature[0], "Temperature", "C", 0, HEATER_0_MAXTEMP - 15);
            lcd_change_to_menu(lcd_menu_material_temperature_settings);
        }
#if TEMP_SENSOR_BED != 0
        else if (IS_SELECTED_SCROLL(2))
            LCD_EDIT_SETTING(material[active_extruder].bed_temperature, "Buildplate Temp.", "C", 0, BED_MAXTEMP - 15);
#endif
        else if (IS_SELECTED_SCROLL(2 + BED_MENU_OFFSET))
            LCD_EDIT_SETTING_FLOAT001(material[active_extruder].diameter, "Material Diameter", "mm", 0, 100);
        else if (IS_SELECTED_SCROLL(3 + BED_MENU_OFFSET))
            LCD_EDIT_SETTING(material[active_extruder].fan_speed, "Fan speed", "%", 0, 100);
        else if (IS_SELECTED_SCROLL(4 + BED_MENU_OFFSET))
            LCD_EDIT_SETTING(material[active_extruder].flow, "Material flow", "%", 1, 1000);
#ifdef USE_CHANGE_TEMPERATURE
        else if (IS_SELECTED_SCROLL(5 + BED_MENU_OFFSET))
            LCD_EDIT_SETTING(material[active_extruder].change_temperature, "Change temperature", "C", 0, HEATER_0_MAXTEMP - 15);
        else if (IS_SELECTED_SCROLL(6 + BED_MENU_OFFSET))
            LCD_EDIT_SETTING(material[active_extruder].change_preheat_wait_time, "Change wait time", "sec", 0, 180);
#endif
        else if (IS_SELECTED_SCROLL(5 + USE_CHANGE_TEMPERATURE_MENU_OFFSET + BED_MENU_OFFSET))
            lcd_change_to_menu(lcd_menu_material_retraction_settings);
        else if (IS_SELECTED_SCROLL(6 + USE_CHANGE_TEMPERATURE_MENU_OFFSET + BED_MENU_OFFSET))
            lcd_change_to_menu(lcd_menu_material_settings_store);
    }
}

static char* lcd_material_temperature_settings_callback(uint8_t nr)
{
    if (nr == 0)
    {
        strcpy_P(card.longFilename, PSTR("< RETURN"));
    }
    else
    {
        strcpy_P(card.longFilename, PSTR("Temperature: "));
        float_to_string(nozzleIndexToNozzleSize(nr - 1), card.longFilename + strlen(card.longFilename));
    }
    return card.longFilename;
}

static void lcd_material_settings_temperature_details_callback(uint8_t nr)
{
    char buffer[10];
    buffer[0] = '\0';
    if (nr == 0)
        return;
    int_to_string(material[active_extruder].temperature[nr - 1], buffer, PSTR("C"));
    lcd_lib_draw_string(5, 53, buffer);
}

static void lcd_menu_material_temperature_settings()
{
    lcd_scroll_menu(PSTR("MATERIAL"), 1 + MATERIAL_NOZZLE_COUNT, lcd_material_temperature_settings_callback, lcd_material_settings_temperature_details_callback);
    if (lcd_lib_button_pressed)
    {
        if (IS_SELECTED_SCROLL(0))
        {
            lcd_change_to_menu(lcd_menu_material_settings);
        }else{
            uint8_t index = SELECTED_SCROLL_MENU_ITEM() - 1;
            LCD_EDIT_SETTING(material[active_extruder].temperature[index], "Temperature", "C", 0, HEATER_0_MAXTEMP - 15);
            previousMenu = lcd_menu_material_settings;
            previousEncoderPos = SCROLL_MENU_ITEM_POS(1);
        }
    }
}

static char* lcd_material_retraction_settings_callback(uint8_t nr)
{
    if (nr == 0)
    {
        strcpy_P(card.longFilename, PSTR("< RETURN"));
    }
    else
    {
        strcpy_P(card.longFilename, PSTR("Retraction: "));
        float_to_string(nozzleIndexToNozzleSize(nr - 1), card.longFilename + strlen(card.longFilename));
    }
    return card.longFilename;
}

static void lcd_material_settings_retraction_details_callback(uint8_t nr)
{
    char buffer[20];
    buffer[0] = '\0';
    if (nr == 0)
        return;
    char* c = float_to_string(material[active_extruder].retraction_length[nr - 1], buffer, PSTR("mm "));
    int_to_string(material[active_extruder].retraction_speed[nr - 1] / 60.0 + 0.5, c, PSTR("mm/s"));
    lcd_lib_draw_string(5, 53, buffer);
}

static void lcd_menu_material_retraction_settings()
{
    lcd_scroll_menu(PSTR("MATERIAL"), 1 + MATERIAL_NOZZLE_COUNT, lcd_material_retraction_settings_callback, lcd_material_settings_retraction_details_callback);
    if (lcd_lib_button_pressed)
    {
        if (IS_SELECTED_SCROLL(0))
        {
            lcd_change_to_menu(lcd_menu_material_settings);
        }else{
            nozzle_select_index = SELECTED_SCROLL_MENU_ITEM() - 1;
            lcd_change_to_menu(lcd_menu_material_retraction_settings_per_nozzle);
        }
    }
}

static char* lcd_retraction_item_per_nozzle(uint8_t nr)
{
    if (nr == 0)
        strcpy_P(card.longFilename, PSTR("< RETURN"));
    else if (nr == 1)
        strcpy_P(card.longFilename, PSTR("Retract length"));
    else if (nr == 2)
        strcpy_P(card.longFilename, PSTR("Retract speed"));
    else
        strcpy_P(card.longFilename, PSTR("???"));
    return card.longFilename;
}

static void lcd_retraction_details_per_nozzle(uint8_t nr)
{
    char buffer[16];
    if (nr == 0)
        return;
    else if(nr == 1)
        float_to_string(material[active_extruder].retraction_length[nozzle_select_index], buffer, PSTR("mm"));
    else if(nr == 2)
        int_to_string(material[active_extruder].retraction_speed[nozzle_select_index] / 60 + 0.5, buffer, PSTR("mm/sec"));
    lcd_lib_draw_string(5, 53, buffer);
}

static void lcd_menu_material_retraction_settings_per_nozzle()
{
    lcd_scroll_menu(PSTR("RETRACTION"), 3, lcd_retraction_item_per_nozzle, lcd_retraction_details_per_nozzle);
    if (lcd_lib_button_pressed)
    {
        if (IS_SELECTED_SCROLL(0))
            lcd_change_to_menu(lcd_menu_material_retraction_settings);
        else if (IS_SELECTED_SCROLL(1))
            LCD_EDIT_SETTING_FLOAT001(material[active_extruder].retraction_length[nozzle_select_index], "Retract length", "mm", 0, 50);
        else if (IS_SELECTED_SCROLL(2))
            LCD_EDIT_SETTING_SPEED(material[active_extruder].retraction_speed[nozzle_select_index], "Retract speed", "mm/sec", 0, max_feedrate[E_AXIS] * 60);
    }
}

static char* lcd_menu_material_settings_store_callback(uint8_t nr)
{
    uint8_t count = eeprom_read_byte(EEPROM_MATERIAL_COUNT_OFFSET());

    if (nr == 0)
        strcpy_P(card.longFilename, PSTR("< RETURN"));
    else if (nr > count)
        strcpy_P(card.longFilename, PSTR("New preset"));
    else{
        eeprom_read_block(card.longFilename, EEPROM_MATERIAL_NAME_OFFSET(nr - 1), MATERIAL_NAME_SIZE);
        card.longFilename[MATERIAL_NAME_SIZE] = '\0';
    }
    return card.longFilename;
}

static void lcd_menu_material_settings_store_details_callback(uint8_t nr)
{
}

static void lcd_menu_material_settings_store()
{
    uint8_t count = eeprom_read_byte(EEPROM_MATERIAL_COUNT_OFFSET());

    if (count == EEPROM_MATERIAL_SETTINGS_MAX_COUNT)
        count--;
    lcd_scroll_menu(PSTR("PRESETS"), 2 + count, lcd_menu_material_settings_store_callback, lcd_menu_material_settings_store_details_callback);

    if (lcd_lib_button_pressed)
    {
        if (!IS_SELECTED_SCROLL(0))
        {
            uint8_t idx = SELECTED_SCROLL_MENU_ITEM() - 1;
            if (idx == count)
            {
                char buffer[MATERIAL_NAME_SIZE + 1] = "CUSTOM";
                int_to_string(idx + 1, buffer + 6);
                strcpy(material[active_extruder].name, buffer);
                eeprom_write_block(buffer, EEPROM_MATERIAL_NAME_OFFSET(idx), MATERIAL_NAME_SIZE);
                eeprom_write_byte(EEPROM_MATERIAL_COUNT_OFFSET(), idx + 1);
            }
            lcd_material_store_material(idx);
        }
        lcd_change_to_menu(lcd_menu_material_settings, SCROLL_MENU_ITEM_POS(6 + USE_CHANGE_TEMPERATURE_MENU_OFFSET + BED_MENU_OFFSET));
    }
}

void lcd_material_reset_defaults()
{
    SERIAL_ECHO_START;
    SERIAL_ECHOLNPGM("lcd_material_reset_defaults");
    //Fill in the defaults
    char buffer[MATERIAL_NAME_SIZE + 1];

    strcpy_P(buffer, PSTR("PLA"));
    eeprom_write_block(buffer, EEPROM_MATERIAL_NAME_OFFSET(0), 4);
    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(0), 210);
    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(0), 60);
    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(0), 100);
    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(0), 100);
    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(0), 2.85);

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(0, 0), 210);//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(0, 1), 195);//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(0, 2), 230);//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(0, 3), 240);//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(0, 4), 240);//1.0

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(0, 0), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(0, 1), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(0, 2), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(0, 3), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(0, 4), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//1.0

    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(0, 0), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.4
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(0, 1), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.25
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(0, 2), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.6
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(0, 3), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.8
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(0, 4), (25 * EEPROM_RETRACTION_SPEED_SCALE));//1.0

    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(0), 70);
    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(0), 30);

    strcpy_P(buffer, PSTR("ABS"));
    eeprom_write_block(buffer, EEPROM_MATERIAL_NAME_OFFSET(1), 4);
    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(1), 260);
    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(1), 80);
    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(1), 100);
    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(1), 107);
    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(1), 2.85);

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(1, 0), 250);//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(1, 1), 245);//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(1, 2), 260);//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(1, 3), 260);//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(1, 4), 260);//1.0

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(1, 0), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(1, 1), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(1, 2), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(1, 3), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(1, 4), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//1.0

    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(1, 0), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.4
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(1, 1), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.25
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(1, 2), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.6
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(1, 3), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.8
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(1, 4), (25 * EEPROM_RETRACTION_SPEED_SCALE));//1.0

    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(1), 90);
    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(1), 30);

    strcpy_P(buffer, PSTR("CPE"));
    eeprom_write_block(buffer, EEPROM_MATERIAL_NAME_OFFSET(2), 4);
    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(2), 250);
    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(2), 70);
    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(2), 100);
    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(2), 100);
    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(2), 2.85);

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(2, 0), 250);//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(2, 1), 245);//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(2, 2), 260);//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(2, 3), 260);//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(2, 4), 260);//1.0

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(2, 0), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(2, 1), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(2, 2), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(2, 3), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(2, 4), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//1.0

    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(2, 0), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.4
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(2, 1), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.25
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(2, 2), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.6
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(2, 3), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.8
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(2, 4), (25 * EEPROM_RETRACTION_SPEED_SCALE));//1.0

    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(2), 85);
    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(2), 15);

    strcpy_P(buffer, PSTR("PC"));
    eeprom_write_block(buffer, EEPROM_MATERIAL_NAME_OFFSET(3), 3);
    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(3), 260);
    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(3), 110);
    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(3), 100);
    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(3), 100);
    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(3), 2.85);

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(3, 0), 260);//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(3, 1), 260);//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(3, 2), 260);//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(3, 3), 260);//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(3, 4), 260);//1.0

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(3, 0), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(3, 1), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(3, 2), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(3, 3), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(3, 4), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//1.0

    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(3, 0), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.4
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(3, 1), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.25
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(3, 2), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.6
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(3, 3), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.8
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(3, 4), (25 * EEPROM_RETRACTION_SPEED_SCALE));//1.0

    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(3), 85);
    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(3), 15);

    strcpy_P(buffer, PSTR("Nylon"));
    eeprom_write_block(buffer, EEPROM_MATERIAL_NAME_OFFSET(4), 6);
    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(4), 250);
    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(4), 60);
    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(4), 100);
    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(4), 100);
    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(4), 2.85);

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(4, 0), 250);//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(4, 1), 240);//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(4, 2), 255);//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(4, 3), 260);//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(4, 4), 260);//1.0

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(4, 0), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(4, 1), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(4, 2), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(4, 3), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(4, 4), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//1.0

    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(4, 0), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.4
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(4, 1), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.25
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(4, 2), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.6
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(4, 3), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.8
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(4, 4), (25 * EEPROM_RETRACTION_SPEED_SCALE));//1.0

    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(4), 85);
    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(4), 15);

    strcpy_P(buffer, PSTR("CPE+"));
    eeprom_write_block(buffer, EEPROM_MATERIAL_NAME_OFFSET(5), 5);
    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(5), 260);
    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(5), 110);
    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(5), 100);
    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(5), 100);
    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(5), 2.85);

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(5, 0), 260);//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(5, 1), 260);//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(5, 2), 260);//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(5, 3), 260);//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(5, 4), 260);//1.0

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(5, 0), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(5, 1), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(5, 2), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(5, 3), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(5, 4), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//1.0

    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(5, 0), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.4
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(5, 1), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.25
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(5, 2), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.6
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(5, 3), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.8
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(5, 4), (25 * EEPROM_RETRACTION_SPEED_SCALE));//1.0

    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(5), 85);
    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(5), 15);

    strcpy_P(buffer, PSTR("TPU 95A"));
    eeprom_write_block(buffer, EEPROM_MATERIAL_NAME_OFFSET(6), 8);
    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(6), 235);
    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(6), 70);
    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(6), 100);
    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(6), 100);
    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(6), 2.85);

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(6, 0), 235);//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(6, 1), 235);//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(6, 2), 235);//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(6, 3), 235);//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(6, 4), 235);//1.0

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(6, 0), (10.0 * EEPROM_RETRACTION_LENGTH_SCALE));//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(6, 1), (10.0 * EEPROM_RETRACTION_LENGTH_SCALE));//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(6, 2), (10.0 * EEPROM_RETRACTION_LENGTH_SCALE));//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(6, 3), (10.0 * EEPROM_RETRACTION_LENGTH_SCALE));//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(6, 4), (10.0 * EEPROM_RETRACTION_LENGTH_SCALE));//1.0

    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(6, 0), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.4
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(6, 1), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.25
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(6, 2), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.6
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(6, 3), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.8
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(6, 4), (25 * EEPROM_RETRACTION_SPEED_SCALE));//1.0

    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(6), 85);
    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(6), 15);

    strcpy_P(buffer, PSTR("PP"));
    eeprom_write_block(buffer, EEPROM_MATERIAL_NAME_OFFSET(7), 3);
    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(7), 220);
    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(7), 100);
    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(7), 100);
    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(7), 100);
    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(7), 2.85);

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(7, 0), 220);//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(7, 1), 220);//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(7, 2), 230);//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(7, 3), 240);//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(7, 4), 240);//1.0

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(7, 0), (8.0 * EEPROM_RETRACTION_LENGTH_SCALE));//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(7, 1), (8.0 * EEPROM_RETRACTION_LENGTH_SCALE));//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(7, 2), (8.0 * EEPROM_RETRACTION_LENGTH_SCALE));//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(7, 3), (8.0 * EEPROM_RETRACTION_LENGTH_SCALE));//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(7, 4), (8.0 * EEPROM_RETRACTION_LENGTH_SCALE));//1.0

    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(7, 0), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.4
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(7, 1), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.25
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(7, 2), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.6
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(7, 3), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.8
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(7, 4), (25 * EEPROM_RETRACTION_SPEED_SCALE));//1.0

    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(7), 85);
    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(7), 15);

    strcpy_P(buffer, PSTR("TPLA"));
    eeprom_write_block(buffer, EEPROM_MATERIAL_NAME_OFFSET(8), 5);
    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(8), 205);
    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(8), 50);
    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(8), 100);
    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(8), 100);
    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(8), 2.85);

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(8, 0), 205);//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(8, 1), 195);//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(8, 2), 210);//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(8, 3), 210);//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(8, 4), 215);//1.0

    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(8, 0), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.4
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(8, 1), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.25
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(8, 2), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.6
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(8, 3), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//0.8
    eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(8, 4), (6.5 * EEPROM_RETRACTION_LENGTH_SCALE));//1.0

    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(8, 0), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.4
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(8, 1), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.25
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(8, 2), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.6
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(8, 3), (25 * EEPROM_RETRACTION_SPEED_SCALE));//0.8
    eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(8, 4), (25 * EEPROM_RETRACTION_SPEED_SCALE));//1.0

    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(8), 70);
    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(8), 30);

    eeprom_write_byte(EEPROM_MATERIAL_COUNT_OFFSET(), 9);

    for(uint8_t m=0; m<5; m++)
    {
        for(uint8_t n=MATERIAL_NOZZLE_COUNT; n<MAX_MATERIAL_NOZZLE_CONFIGURATIONS; n++)
        {
            eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(m, n), 0);

            eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(m, n), (0.0 * EEPROM_RETRACTION_LENGTH_SCALE));
            eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(m, n), (0 * EEPROM_RETRACTION_SPEED_SCALE));
        }
    }
}

void lcd_material_set_material(uint8_t nr, uint8_t e)
{
    material[e].temperature[0] = eeprom_read_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(nr));
#if TEMP_SENSOR_BED != 0
    material[e].bed_temperature = eeprom_read_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(nr));
#endif
    material[e].flow = eeprom_read_word(EEPROM_MATERIAL_FLOW_OFFSET(nr));

    material[e].fan_speed = eeprom_read_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(nr));
    material[e].diameter = eeprom_read_float(EEPROM_MATERIAL_DIAMETER_OFFSET(nr));
    eeprom_read_block(material[e].name, EEPROM_MATERIAL_NAME_OFFSET(nr), MATERIAL_NAME_SIZE);
    material[e].name[MATERIAL_NAME_SIZE] = '\0';
    strcpy(card.longFilename, material[e].name);
    for(uint8_t n=0; n<MAX_MATERIAL_NOZZLE_CONFIGURATIONS; n++)
    {
        material[e].temperature[n] = eeprom_read_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(nr, n));
        if (material[e].temperature[n] > HEATER_0_MAXTEMP - 15)
            material[e].temperature[n] = HEATER_0_MAXTEMP - 15;
        material[e].retraction_length[n] = float(eeprom_read_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(nr, n))) / EEPROM_RETRACTION_LENGTH_SCALE;
        material[e].retraction_speed[n] = float(eeprom_read_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(nr, n))) / EEPROM_RETRACTION_SPEED_SCALE * 60;
    }
#if TEMP_SENSOR_BED != 0
    if (material[e].bed_temperature > BED_MAXTEMP - 15)
        material[e].bed_temperature = BED_MAXTEMP - 15;
#endif
    material[e].change_temperature = eeprom_read_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(nr));
    material[e].change_preheat_wait_time = eeprom_read_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(nr));
    if (material[e].change_temperature < 10)
        material[e].change_temperature = material[e].temperature[0];

    lcd_material_store_current_material();
}

void lcd_material_store_material(uint8_t nr)
{
    eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(nr), material[active_extruder].temperature[0]);
#if TEMP_SENSOR_BED != 0
    eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(nr), material[active_extruder].bed_temperature);
#endif
    eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(nr), material[active_extruder].flow);

    eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(nr), material[active_extruder].fan_speed);
    eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(nr), material[active_extruder].diameter);
    //eeprom_write_block(card.longFilename, EEPROM_MATERIAL_NAME_OFFSET(nr), MATERIAL_NAME_SIZE);
    for(uint8_t n=0; n<MAX_MATERIAL_NOZZLE_CONFIGURATIONS; n++)
    {
        eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(nr, n), material[active_extruder].temperature[n]);

        eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(nr, n), material[active_extruder].retraction_length[n] * EEPROM_RETRACTION_LENGTH_SCALE);
        eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(nr, n), material[active_extruder].retraction_speed[n] / 60.0 * EEPROM_RETRACTION_SPEED_SCALE);
    }

    eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(nr), material[active_extruder].change_temperature);
    eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(nr), material[active_extruder].change_preheat_wait_time);
}

void lcd_material_read_current_material()
{
    for(uint8_t e=0; e<EXTRUDERS; e++)
    {
        //material[e].temperature[0] = eeprom_read_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e));
#if TEMP_SENSOR_BED != 0
        material[e].bed_temperature = eeprom_read_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e));
#endif
        material[e].flow = eeprom_read_word(EEPROM_MATERIAL_FLOW_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e));

        material[e].fan_speed = eeprom_read_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e));
        material[e].diameter = eeprom_read_float(EEPROM_MATERIAL_DIAMETER_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e));

        for(uint8_t n=0; n<MAX_MATERIAL_NOZZLE_CONFIGURATIONS; n++)
        {
            material[e].temperature[n] = eeprom_read_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e, n));
            material[e].retraction_length[n] = float(eeprom_read_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e, n))) / EEPROM_RETRACTION_LENGTH_SCALE;
            material[e].retraction_speed[n] = float(eeprom_read_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e, n))) / EEPROM_RETRACTION_SPEED_SCALE * 60;
        }

        eeprom_read_block(material[e].name, EEPROM_MATERIAL_NAME_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e), MATERIAL_NAME_SIZE);
        material[e].name[MATERIAL_NAME_SIZE] = '\0';

        material[e].change_temperature = eeprom_read_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e));
        material[e].change_preheat_wait_time = eeprom_read_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e));
        if (material[e].change_temperature < 10)
            material[e].change_temperature = material[e].temperature[0];
    }
}

void lcd_material_store_current_material()
{
    for(uint8_t e=0; e<EXTRUDERS; e++)
    {
        eeprom_write_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e), material[e].temperature[0]);
#if TEMP_SENSOR_BED != 0
        eeprom_write_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e), material[e].bed_temperature);
#endif
        eeprom_write_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e), material[e].fan_speed);
        eeprom_write_word(EEPROM_MATERIAL_FLOW_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e), material[e].flow);
        eeprom_write_float(EEPROM_MATERIAL_DIAMETER_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e), material[e].diameter);
        for(uint8_t n=0; n<MAX_MATERIAL_NOZZLE_CONFIGURATIONS; n++)
        {
            eeprom_write_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e, n), material[active_extruder].temperature[n]);
            eeprom_write_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e, n), material[active_extruder].retraction_length[n] * EEPROM_RETRACTION_LENGTH_SCALE);
            eeprom_write_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e, n), material[active_extruder].retraction_speed[n] / 60.0 * EEPROM_RETRACTION_SPEED_SCALE);
        }

        eeprom_write_block(material[e].name, EEPROM_MATERIAL_NAME_OFFSET(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e), MATERIAL_NAME_SIZE);

        eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e), material[e].change_temperature);
        eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(EEPROM_MATERIAL_SETTINGS_MAX_COUNT+e), material[e].change_preheat_wait_time);
    }
}

static bool hasInvalidNozzleTemperature(uint16_t temperature)
{
    return temperature == 0 || temperature > HEATER_0_MAXTEMP;
}

static bool hasInvalidBedTemperature(uint16_t temperature)
{
    return temperature > BED_MAXTEMP;
}

static bool hasInvalidFanSpeed(uint8_t fanspeed)
{
    return fanspeed > 100;
}

static bool hasInvalidMaterialFlow(uint16_t flow)
{
    return flow == 0 || flow > 1000;
}

static bool hasInvalidDiameter(float diameter)
{
    return diameter < 0.1 || diameter > 10.0;
}

static bool hasInvalidRetractionLength(uint16_t length)
{
    //More than 20mm retraction is not a valid value
    return length > (20 * EEPROM_RETRACTION_LENGTH_SCALE);
}

static bool hasInvalidRetractionSpeed(uint16_t speed)
{
    //More than 45mm/s is not a valid value
    return speed == 0 || speed > (45 * EEPROM_RETRACTION_SPEED_SCALE);
}

bool lcd_material_verify_material_settings()
{
    uint8_t cnt = eeprom_read_byte(EEPROM_MATERIAL_COUNT_OFFSET());
    if (cnt < 2 || cnt > EEPROM_MATERIAL_SETTINGS_MAX_COUNT)
        return false;
    while(cnt > 0)
    {
        cnt --;
        if (hasInvalidNozzleTemperature(eeprom_read_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(cnt))))
            return false;
#if TEMP_SENSOR_BED != 0
        if (hasInvalidBedTemperature(eeprom_read_word(EEPROM_MATERIAL_BED_TEMPERATURE_OFFSET(cnt))))
            return false;
#endif
        if (hasInvalidFanSpeed(eeprom_read_byte(EEPROM_MATERIAL_FAN_SPEED_OFFSET(cnt))))
            return false;
        if (hasInvalidMaterialFlow(eeprom_read_word(EEPROM_MATERIAL_FLOW_OFFSET(cnt))))
            return false;
        if (hasInvalidDiameter(eeprom_read_float(EEPROM_MATERIAL_DIAMETER_OFFSET(cnt))))
            return false;

        for(uint8_t n=0; n<MATERIAL_NOZZLE_COUNT; n++)
        {
            if (hasInvalidNozzleTemperature(eeprom_read_word(EEPROM_MATERIAL_EXTRA_TEMPERATURE_OFFSET(cnt, n))))
                return false;
            if (hasInvalidRetractionLength(eeprom_read_word(EEPROM_MATERIAL_EXTRA_RETRACTION_LENGTH_OFFSET(cnt, n))))
                return false;
            if (hasInvalidRetractionSpeed(eeprom_read_byte(EEPROM_MATERIAL_EXTRA_RETRACTION_SPEED_OFFSET(cnt, n))))
                return false;
        }

        if (hasInvalidNozzleTemperature(eeprom_read_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(cnt))))
        {
            //Invalid temperature for change temperature.
            if (strcmp_P(card.longFilename, PSTR("PLA")) == 0)
            {
                eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(cnt), 70);
                eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(cnt), 30);
            }else if (strcmp_P(card.longFilename, PSTR("ABS")) == 0)
            {
                eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(cnt), 90);
                eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(cnt), 30);
            }else if (strcmp_P(card.longFilename, PSTR("CPE")) == 0)
            {
                eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(cnt), 85);
                eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(cnt), 15);
            }else{
                eeprom_write_word(EEPROM_MATERIAL_CHANGE_TEMPERATURE(cnt), eeprom_read_word(EEPROM_MATERIAL_TEMPERATURE_OFFSET(cnt)));
                eeprom_write_byte(EEPROM_MATERIAL_CHANGE_WAIT_TIME(cnt), 5);
            }
        }
    }
    return true;
}

uint8_t nozzleSizeToTemperatureIndex(float nozzle_size)
{
    if (fabs(nozzle_size - 0.25) < 0.05)
        return 1;
    if (fabs(nozzle_size - 0.60) < 0.1)
        return 2;
    if (fabs(nozzle_size - 0.80) < 0.1)
        return 3;
    if (fabs(nozzle_size - 1.00) < 0.1)
        return 4;

    //Default to index 0
    return 0;
}

float nozzleIndexToNozzleSize(uint8_t nozzle_index)
{
    switch(nozzle_index)
    {
    case 0:
        return 0.4;
    case 1:
        return 0.25;
    case 2:
        return 0.6;
    case 3:
        return 0.8;
    case 4:
        return 1.0;
    }
    return 0.0;
}

#endif//ENABLE_ULTILCD2
