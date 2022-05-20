#ifndef HARDWARE_H
#define HARDWARE_H

#include <Arduino.h>
#include <U8g2lib.h>

#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/pwm.h"
#include "fpmath.h"

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

#define NUM_WORDS 3

uint TOP_BTN_CCW[] = {0, 16, 21};
uint ENC_BTN_CW[]  = {1, 17, 22};
uint TRIG_IN[]     = {18, 19, 20};
uint CV_IN[]       = {26, 27, 28};
uint VOCT_OFFSET[] = {2,  6,  8};
uint CV_OFFSET[]   = {10, 12, 14};

class GateTrigger {
public:
  uint pin;
  bool state;
  bool fallingEdge;
  bool risingEdge;
  GateTrigger(uint pin) {
    this->pin = pin;
    this->state = false;
    this->fallingEdge = false;
    this->risingEdge = false;
    gpio_pull_up(pin);
  }
  void Update() {
    bool newState = !gpio_get(pin);
    if(newState > state) this->risingEdge = true;
    if(newState < state) this->fallingEdge = true;
    this->state = newState; 
  }
  bool State() { return state; }
  bool FallingEdge() {
    if(fallingEdge) {
      fallingEdge = false;
      return true;
    }
    return false;
  }
  bool RisingEdge() {
    if(risingEdge) {
      risingEdge = false;
      return true;
    }
    return false;
  }
};

class ButtonAndEncoder {
private:
  bool _topButtonPressed;
  bool _encButtonPressed;
public:
  enum State {
    NONE,
    TOP_CCW_TRIG,
    ENC_CW_TRIG
  };
  uint topButtonCCW;
  uint encButtonCW;
  int encValue;
  State state;
  uint64_t nextCanTrigger;
  uint64_t delayTime;
  bool topButtonHeld;
  bool encButtonHeld;
  bool topButtonPressed() {
    bool out = _topButtonPressed;
    _topButtonPressed = false;
    return out;
  }
  bool encButtonPressed() {
    bool out = _encButtonPressed;
    _encButtonPressed = false;
    return out;
  }
  ButtonAndEncoder(uint topButtonCcw, uint encButtonCw) {
    this->topButtonCCW = topButtonCcw;
    this->encButtonCW = encButtonCw;
    this->encValue = 0;
    this->state = NONE;
    this->nextCanTrigger = time_us_64();
    this->delayTime = 1000000/40;
    this->topButtonHeld = false;
    this->_topButtonPressed = false;
    this->encButtonHeld = false;
    this->_encButtonPressed = false;
    gpio_pull_up(topButtonCCW);
    gpio_pull_up(encButtonCW);
  }

  int GetDelta() {
    int delta = encValue;
    encValue = 0;
    return delta;
  }

  void Update() {
    if(time_us_64() > (nextCanTrigger + delayTime)) {
      bool topButtonState = !gpio_get(topButtonCCW);
      bool encButtonState = !gpio_get(encButtonCW);
      if(state != NONE) {
        _topButtonPressed |= !topButtonHeld && topButtonState;
        _encButtonPressed |= !encButtonHeld && encButtonState;
      }
      topButtonHeld = topButtonState;
      encButtonHeld = encButtonState;
      state = NONE;
    }
  }
  void Update(uint pin, uint32_t events) {
    switch(state) {
      case NONE:
        if(time_us_64() > nextCanTrigger) {
          if(pin == topButtonCCW && !gpio_get(pin)) {
            state = TOP_CCW_TRIG;
            nextCanTrigger = time_us_64() + delayTime;
          }
          if(pin == encButtonCW && !gpio_get(pin)) {
            state = ENC_CW_TRIG;
            nextCanTrigger = time_us_64() + delayTime;
          }
        }
        break;
      case TOP_CCW_TRIG:
        if(pin == encButtonCW && !gpio_get(pin)) {
          encValue++;
          state = NONE;
          nextCanTrigger = time_us_64() + delayTime;
        }
        break;
      case ENC_CW_TRIG:
        if(pin == topButtonCCW && !gpio_get(pin)) {
          encValue--;
          state = NONE;
          nextCanTrigger = time_us_64() + delayTime;
        }
        break;
    }
  }

};

class AnalogOut {
public:
  uint16_t res;
  uint offset;
  AnalogOut(int offset, int resolution = 255) {
    this->offset = offset;
    this->res = resolution;
    for(uint16_t i=0;i<2;i++) {
      uint slice_num = pwm_gpio_to_slice_num(i + offset);
      pwm_config cfg = pwm_get_default_config();
      pwm_config_set_clkdiv_int(&cfg, 1);
      pwm_config_set_wrap(&cfg, this->res);
      pwm_init(slice_num, &cfg, true);
      gpio_set_function(i + offset, GPIO_FUNC_PWM);
      pwm_set_gpio_level(i + offset, 0);
    }
  }
  void Set(uint pin, double level) {
    pwm_set_gpio_level(pin, (uint16_t)(level*this->res));
  }
  void SetOutputVoltage(double v) {
    Set(offset, ((VOCT_NOUT_MAX-v))/VOCT_NOUT_MAX);
  }
  void SetOffsetVoltage(double v) {
    Set(offset + 1, (v)/VOCT_POUT_MAX);
  }
};

class TLWHardware {
public:
  static TLWHardware* _tlwhw_;
  static void (*_audioCallback_)(void);
  struct repeating_timer _timer_;

  U8G2_SSD1306_128X64_NONAME_F_HW_I2C* display;
  ButtonAndEncoder* control[NUM_WORDS];
  GateTrigger* trigIn[NUM_WORDS];
  AnalogOut* voctOut[NUM_WORDS];
  AnalogOut* cvOut[NUM_WORDS];

  static void controlHandler(uint gpio, uint32_t events) {
    for(int i=0; i<NUM_WORDS; i++) {
      _tlwhw_->control[i]->Update(gpio, events);
    }
  }

  static bool audioHandler(struct repeating_timer *t) {
    if(_audioCallback_ != NULL) _audioCallback_();
    return true;
  }

  void Init(void (*audioCallback)(void)) {
    if(_tlwhw_ == NULL) {
      display = new U8G2_SSD1306_128X64_NONAME_F_HW_I2C(U8G2_R0, U8X8_PIN_NONE, 5, 4);
      display->setBusClock(400000);
      display->begin();
      for(int i=0; i<NUM_WORDS; i++) {
        control[i] = new ButtonAndEncoder(TOP_BTN_CCW[i], ENC_BTN_CW[i]);
        gpio_set_irq_enabled_with_callback(TOP_BTN_CCW[i], GPIO_IRQ_EDGE_FALL, true, &controlHandler);
        gpio_set_irq_enabled_with_callback(ENC_BTN_CW[i], GPIO_IRQ_EDGE_FALL, true, &controlHandler);
        trigIn[i]   = new GateTrigger(TRIG_IN[i]);
        voctOut[i]  = new AnalogOut(VOCT_OFFSET[i]);
        cvOut[i]    = new AnalogOut(CV_OFFSET[i]);
      }
      this->_audioCallback_ = audioCallback;
      add_repeating_timer_us(-TIMER_INTERVAL, audioHandler, NULL, &_timer_);
      _tlwhw_ = this;
    }
  }

  void Update() {
    for(int i=0; i<NUM_WORDS; i++) {
      control[i]->Update();
    }
  }
};
TLWHardware* TLWHardware::_tlwhw_ = NULL;
void (*TLWHardware::_audioCallback_)(void) = NULL;

#endif