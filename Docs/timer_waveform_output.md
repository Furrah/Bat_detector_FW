# Timer waveform output (PWM on a GPIO)

V1 generated the square-wave LO for its analogue mixer with TIM3 CH1 on PB4. V2 mixes digitally, so this is being removed from the V2 firmware. It's kept here for future builds, since a timer channel is the cheapest way to get a stable, CPU-free waveform on a pin.

The last full working version is commit `cd8940b` ("V1 of firmware complete"):
- `Core/Drivers/tim3_lo.c` / `tim3_lo.h`
- `MX_TIM3_Init()` in `Core/Src/main.c`
- `HAL_TIM_MspPostInit()` in `Core/Src/stm32g4xx_hal_msp.c`

## How it works

The counter runs from 0 to ARR and restarts. The output is high while the count is below CCR (PWM mode 1).

- f_out = f_TIM / ((PSC + 1) × (ARR + 1))
- duty = CCR / (ARR + 1)
- f_TIM = 170 MHz for TIM2/TIM3/TIM4. The APB1 timer clock stays at 170 MHz even with APB1 at /2, because timer clocks are doubled when the APB prescaler isn't 1.

## CubeMX setup (as used on V1)

1. Timers → TIM3:
   - Clock Source = Internal Clock.
   - Channel1 = PWM Generation CH1.
2. Parameter Settings:
   - Prescaler = 0, Counter Mode = Up.
   - Counter Period (ARR) = f_TIM / f − 1. V1 used 3777, which gives 45.0 kHz.
   - auto-reload preload = Enable.
   - PWM Generation Channel 1: Mode = PWM mode 1, Pulse = (ARR + 1) / 2 (1889), polarity High.
3. The pin's alternate function comes from the pin assignment: PB4 = `GPIO_AF2_TIM3`. CubeMX writes this into `HAL_TIM_MspPostInit()`.

## Code (V1, proven on hardware)

Start it once after `MX_TIM3_Init()`:

```c
HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
```

Retune at run time (`tim3_lo.c`):

```c
void LO_SetFrequency(uint32_t frequency)
{
    uint32_t arr;

    if(frequency < 1000)
        frequency = 1000;

    if(frequency > 90000)
        frequency = 90000;

    arr = (170000000UL / frequency) - 1;

    __HAL_TIM_DISABLE(&htim3);
    __HAL_TIM_SET_AUTORELOAD(&htim3, arr);
    __HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, arr/2);   // keep ~50% duty
    __HAL_TIM_ENABLE(&htim3);
}
```

## Limits and caveats

- **TIM3 is 16-bit.** Only TIM2 is 32-bit on the G431. With PSC = 0, ARR ≤ 65535, so the lowest frequency is 170 MHz / 65536 ≈ 2.6 kHz.
  - V1's 1 kHz clamp would have overflowed: ARR = 169,999 is stored as 38,927, giving about 4.4 kHz instead.
  - For lower frequencies, raise PSC or use TIM2.
- **Stopping the timer to retune isn't needed.** With ARR preload enabled (ARPE) and CCR preload enabled (OC1PE, set by `HAL_TIM_PWM_ConfigChannel`), both new values load together at the next update event. The disable/enable pair only stalls the output briefly.
- **Frequency resolution** is Δf ≈ f² / f_TIM: about 12 Hz at 45 kHz and about 48 Hz at 90 kHz.
  - G4 timers have a dithering mode (`TIMx_CR1.DITHEN`; CubeMX's "PeriodNoDither" field relates to it). It gives ARR/CCR a 4-bit fractional part, so the average resolution is about 16× finer, at the cost of cycle-to-cycle jitter.
  - See RM0440, "Dithering mode".
- **The duty cycle is exactly 50% only when ARR + 1 is even.**
- **A square wave carries odd harmonics** (3f, 5f, …). Used as a mixer LO, each one mixes with the input too.

## Pins with a timer channel on the V2 board

None of these are connected on V2, so using one needs a board change. The data comes from CubeMX's database (`STM32G431C(6-8-B)Tx.xml` and `GPIO-STM32G43x_gpio_v1_0_Modes.xml`). Confirm it against the datasheet's alternate-function table before laying out.

| Pin | Timer channels (AF) |
|---|---|
| PA7 | TIM3_CH2 (AF2), TIM17_CH1 (AF1), TIM1_CH1N (AF6), TIM8_CH1N (AF4) |
| PA10 | TIM2_CH4 (AF10), TIM1_CH3 (AF6) |
| PA15 | TIM2_CH1 (AF1), TIM8_CH1 (AF2) |
| PB1 | TIM3_CH4 (AF2), TIM1_CH3N (AF6), TIM8_CH3N (AF4) |
| PB9 | TIM4_CH4 (AF2), TIM17_CH1 (AF1), TIM8_CH3 (AF10), TIM1_CH3N (AF12) |
| PC13 | TIM1_CH1N (AF4), TIM8_CH4N (AF6) |

Notes:
- **TIM2 is the ADC/DAC sample clock.** A TIM2 channel on PA10 or PA15 would output a waveform locked to the sample rate, which is useful as a scope trigger. Set it with a compare channel and never change TIM2's ARR.
- **`CHxN` outputs are the complementary outputs of the advanced timers TIM1 and TIM8.** They need the main output enabled and are started with `HAL_TIMEx_PWMN_Start()`.
- **PC13 is in the backup-domain I/O group, with limited drive and speed.** Prefer another pin.
