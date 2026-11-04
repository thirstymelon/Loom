#!/usr/bin/env python3
import pathlib
import subprocess
import sys
import tempfile

compiler = pathlib.Path(sys.argv[1]).resolve()
repo_root = pathlib.Path(__file__).resolve().parent.parent
bsp_dir = repo_root / "sdk" / "targets" / "rp2040" / "bsp"

print(f"Testing RP2040 peripherals in {bsp_dir} using {compiler}...")

modules = [
    ("System", """
Module TestSystem
Import System
use System
Block Main Declares
    Rev : Signed_64
    Part : Signed_64
    Asic : Boolean
Implements
    System_Init()
    Rev = Get_Chip_Revision()
    Part = Get_Chip_Part()
    Asic = Is_ASIC_Platform()
    Unreset_Peripherals(RESET_UART0 | RESET_SPI0 | RESET_I2C0)
    Reset_Peripherals(RESET_PWM)
    If Is_Peripheral_Ready(RESET_TIMER)
        System_Reset()
    End If
End Main
"""),
    ("Clock", """
Module TestClock
Import Clock
use Clock
Block Main Declares
    Stable : Boolean
    RandW : Unsigned_32
    RandB : Signed_64
    Khz : Signed_64
    Mhz : Signed_64
    Resussed : Boolean
    Ok : Boolean
Implements
    XOSC_Init()
    Stable = XOSC_Is_Stable()
    ROSC_Set_Div(16)
    RandB = ROSC_Get_Random_Bit()
    RandW = ROSC_Get_Random_Word()
    Clock_Init()
    Clock_Set_Sys_Freq(125000000)
    Clock_Gpio_Init(21, CLK_AUX_PLL_SYS, 10)
    Khz = Clock_Frequency_Count_Khz(FC_SRC_CLK_SYS)
    Mhz = Clock_Frequency_Count_Mhz(FC_SRC_CLK_SYS)
    Clock_Resus_Enable(100)
    Resussed = Clock_Resus_Has_Triggered()
    Clock_Stop(CLK_ADC)
    Clock_Start(CLK_ADC)
End Main
"""),
    ("GPIO", """
Module TestGPIO
Import GPIO
use GPIO
Block Main Implements
    Set_Function(16, GPIO_FUNC_SPI)
    Init_Pin(25)
    Set_Direction(25, PIN_OUTPUT)
    Set_High(25)
    Set_Low(25)
    Toggle(25)
    Put(25, True)
    If (Read_Pin(25) == 1)
        Set_Pull(25, PULL_UP)
        Set_Drive_Strength(25, DRIVE_4MA)
        Set_Slew_Rate(25, True)
        Set_Schmitt_Trigger(25, True)
        Set_Pins_Mask(0x02000000)
        Clear_Pins_Mask(0x02000000)
        XOR_Pins_Mask(0x02000000)
        Write_Pins_Mask(0x02000000, 0x02000000)
        Enable_Pin_Interrupt(25, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL)
        Enable_Pin_Interrupt_Core(1, 25, GPIO_IRQ_EDGE_RISE)
        Clear_Pin_Interrupt(25, GPIO_IRQ_EDGE_RISE)
        If (Get_Pin_Interrupt_Status(25) != 0)
            Set_Low(25)
        End If
        If (Get_Pin_Interrupt_Status_Core(1, 25) != 0)
            Set_Low(25)
        End If
    End If
End Main
"""),
    ("Time", """
Module TestTime
Import Time
use Time
Block Main Declares
    Us : Signed_64
    Us64 : Signed_64
    Ms : Signed_64
Implements
    Us = Get_Time_Microseconds()
    Us64 = Get_Time_64_Microseconds()
    Ms = Get_Time()
    Delay(1)
    Delay_Microseconds(100)
    SleepMs(1)
    SleepUs(50)
    Set_Alarm_Target(ALARM_0, 1000000)
    Set_Alarm_Delay(ALARM_1, 50000)
    If Is_Alarm_Armed(ALARM_0)
        Clear_Alarm_Interrupt(ALARM_0)
        Enable_Alarm_Interrupt(ALARM_0)
        Disarm_Alarm(ALARM_0)
    End If
End Main
"""),
    ("UART", r"""
Module TestUART
Import UART
use UART
Block Main Declares
    Avail : Boolean
    Writable : Boolean
    Ch : Character
    B : Signed_64
Implements
    UART_Init(BAUD_115200)
    UART_Set_Baud_Rate(UART_0, 9600, 125000000)
    UART_Init_Channel(UART_1, BAUD_9600, 4, 5)
    Write_UART_Byte(65)
    Write_UART("Hello UART!\n")
    UART_Write_Byte_Ch(UART_1, 66)
    UART_Write_String_Ch(UART_1, "UART1\n")
    Avail = UART_Available()
    Writable = UART_Is_Writable()
    If (UART_Is_Available_Ch(UART_1) == True)
        B = UART_Read_Byte_Ch(UART_1)
    End If
End Main
"""),
    ("SPI", """
Module TestSPI
Import SPI
use SPI
Block Main Declares
    Busy : Boolean
    Writable : Boolean
    Readable : Boolean
    B : Signed_64
    TxBuf : Unsigned_8 Array[4]
    RxBuf : Unsigned_8 Array[4]
Implements
    SPI_Init(1000000)
    SPI_Set_Baud_Rate(SPI_0, 2000000, 125000000)
    SPI_Init_Channel(SPI_1, 500000, 10, 11, 12)
    SPI_Set_Format(SPI_0, 8, SPI_CPOL_0, SPI_CPHA_0)
    Busy = SPI_Is_Busy(SPI_0)
    Writable = SPI_Is_Writable(SPI_0)
    Readable = SPI_Is_Readable(SPI_0)
    SPI_Write_Byte_Ch(SPI_0, 0x9F)
    B = SPI_Read_Byte_Ch(SPI_0)
    B = SPI_Transfer_Byte_Ch(SPI_0, 0x00)
    TxBuf[1] = 0x06
    TxBuf[2] = 0x02
    SPI_Write_Slice(SPI_0, Slice(TxBuf))
    SPI_Read_Slice(SPI_0, Slice(RxBuf), 0x00)
    SPI_Transfer_Slices(SPI_0, Slice(TxBuf), Slice(RxBuf))
End Main
"""),
    ("I2C", """
Module TestI2C
Import I2C
use I2C
Block Main Declares
    Ok : Boolean
    Val : Signed_64
    Buf : Unsigned_8 Array[2]
Implements
    I2C_Init(I2C_FAST_400K)
    I2C_Init_Channel(I2C_1, I2C_STANDARD_100K, 2, 3)
    I2C_Init_Channel_Freq(I2C_1, 100000, 2, 3, 125000000)
    Ok = I2C_Write_Byte_Direct(I2C_0, 0x68, 0x6B, True)
    Val = I2C_Read_Byte_Direct(I2C_0, 0x68, True)
    Buf[1] = 0x1A
    Buf[2] = 0x03
    Ok = I2C_Write_Slice(I2C_0, 0x68, Slice(Buf), False)
    Ok = I2C_Read_Slice(I2C_0, 0x68, Slice(Buf), False)
    Ok = I2C_Write_Register_8(I2C_0, 0x68, 0x6B, 0x00)
    Val = I2C_Read_Register_8(I2C_0, 0x68, 0x75)
End Main
"""),
    ("ADC", """
Module TestADC
Import ADC
use ADC
Block Main Declares
    Raw : Signed_64
    Mv : Signed_64
    TempC : Signed_64
    TempMC : Signed_64
Implements
    ADC_Init()
    ADC_Init_Pin(26)
    ADC_Select_Input(ADC_GPIO_26)
    Raw = Read_ADC_Raw(ADC_GPIO_26)
    Mv = Read_ADC_Millivolts(ADC_GPIO_26)
    TempC = Read_Temperature_Celsius()
    TempMC = Read_Temperature_Millicelsius()
End Main
"""),
    ("PWM", """
Module TestPWM
Import PWM
use PWM
Block Main Implements
    PWM_Init(25)
    PWM_Set_Clock_Div(25, 4, 0)
    PWM_Set_Wrap(25, 10000)
    PWM_Set_Enabled(25, True)
    PWM_Set_Mask_Enabled(1)
    Set_PWM_Raw(25, 5000)
    Set_PWM_Duty(25, 75)
End Main
"""),
    ("DMA", """
Module TestDMA
Import DMA
use DMA
Block Main Declares
    Busy : Boolean
    Src : Unsigned_32 Array[4]
    Dst : Unsigned_32 Array[4]
Implements
    DMA_Init()
    DMA_Channel_Configure(0, 0x20000000, 0x20000010, 4, DMA_SIZE_32, True, True, DREQ_UNPACED, 0)
    DMA_Channel_Set_Read_Addr(0, 0x20000000)
    DMA_Channel_Set_Write_Addr(0, 0x20000010)
    DMA_Channel_Set_Trans_Count(0, 4)
    DMA_Start_Multi_Channel(1)
    DMA_Channel_Start(0)
    Busy = DMA_Channel_Is_Busy(0)
    DMA_Channel_Wait_Blocking(0)
    DMA_Channel_Abort(0)
    Src[1] = 100
    DMA_Memcpy_32(Cast[Signed_64](Address(Dst[1])), Cast[Signed_64](Address(Src[1])), 4)
End Main
"""),
    ("Watchdog", """
Module TestWatchdog
Import Watchdog
use Watchdog
Block Main Declares
    Caused : Boolean
    Scratch : Unsigned_32
Implements
    Watchdog_Enable(5000, True)
    Watchdog_Feed()
    Caused = Watchdog_Caused_Reboot()
    Watchdog_Set_Scratch(0, 0x12345678)
    Scratch = Watchdog_Get_Scratch(0)
    If (Caused == True)
        Watchdog_Reboot_To_Bootsel()
    End If
End Main
"""),
    ("Multicore", """
Module TestMulticore
Import Multicore
use Multicore
Block Main Declares
    CoreId : Signed_64
    Locked : Boolean
    Ready : Boolean
    HasData : Boolean
    Msg : Unsigned_32
Implements
    CoreId = Get_Core_Num()
    Locked = Spinlock_Try_Claim(0)
    If (Locked == True)
        Spinlock_Release(0)
    End If
    Spinlock_Claim(1)
    Spinlock_Release(1)
    Ready = Multicore_FIFO_Is_Ready()
    HasData = Multicore_FIFO_Has_Data()
    Multicore_FIFO_Drain()
End Main
"""),
    ("PIO", """
Module TestPIO
Import PIO
use PIO
Block Main Declares
    Full : Boolean
    Empty : Boolean
    Data : Unsigned_32
    Prog : Unsigned_16 Array[2]
Implements
    PIO_Init(PIO_0)
    PIO_Init(PIO_1)
    PIO_Load_Instruction(PIO_0, 0, PIO_INSTR_NOP)
    Prog[1] = PIO_INSTR_NOP
    Prog[2] = PIO_INSTR_NOP
    PIO_Load_Program(PIO_0, 0, Slice(Prog))
    PIO_SM_Init(PIO_0, 0, 0, 16, 1)
    PIO_SM_Set_Clock_Div(PIO_0, 0, 4, 0)
    PIO_SM_Set_Enabled(PIO_0, 0, True)
    Full = PIO_SM_Is_TX_Full(PIO_0, 0)
    Empty = PIO_SM_Is_RX_Empty(PIO_0, 0)
End Main
"""),
    ("RTC", """
Module TestRTC
Import RTC
use RTC
Block Main Declares
    Running : Boolean
    Y : Signed_64
    M : Signed_64
    D : Signed_64
    H : Signed_64
    Min : Signed_64
    S : Signed_64
    W : Signed_64
Implements
    RTC_Init()
    RTC_Set_DateTime(2026, 11, 4, 3, 12, 30, 0)
    Running = RTC_Is_Running()
    Y = RTC_Get_Year()
    M = RTC_Get_Month()
    D = RTC_Get_Day()
    H = RTC_Get_Hour()
    Min = RTC_Get_Minute()
    S = RTC_Get_Second()
    W = RTC_Get_Day_Of_Week()
End Main
"""),
    ("Interp", """
Module TestInterp
Import Interp
use Interp
Block Main Declares
    Res : Unsigned_32
    Comb : Unsigned_32
Implements
    Interp_Set_Accum(INTERP_0, LANE_0, 0)
    Interp_Add_Accum(INTERP_0, LANE_0, 10)
    Interp_Set_Base(INTERP_0, LANE_0, 100)
    Interp_Set_Base_2(INTERP_0, 200)
    Interp_Set_Base_1And0(INTERP_0, 0x1234, 0x5678)
    Interp_Set_Config(INTERP_0, LANE_0, 0, 0, 31, False, False)
    Res = Interp_Peek_Result(INTERP_0, LANE_0)
    Res = Interp_Pop_Result(INTERP_0, LANE_0)
    Comb = Interp_Peek_Combined(INTERP_0)
    Comb = Interp_Pop_Combined(INTERP_0)
End Main
"""),
    ("RP2040_Umbrella", r"""
Module TestRP2040Umbrella
Import RP2040
use RP2040
Block Main Declares
    Now : Signed_64
    Temp : Signed_64
    Core : Signed_64
Implements
    RP2040_Init()
    RP2040_Init_Minimal()
    Core = Get_Core_Num()
    Init_Pin(25)
    Set_Direction(25, PIN_OUTPUT)
    Set_High(25)
    UART_Init(BAUD_115200)
    Write_UART("All RP2040 peripherals running!\n")
    SPI_Init(1000000)
    I2C_Init(I2C_FAST_400K)
    ADC_Init()
    Temp = Read_Temperature_Celsius()
    PWM_Init(15)
    Set_PWM_Duty(15, 50)
    DMA_Init()
    RTC_Init()
    Now = Get_Time()
    Delay(10)
    Set_Low(25)
End Main
""")
]

for name, code in modules:
    with tempfile.NamedTemporaryFile(suffix=".loom", mode="w", delete=False) as f:
        f.write(code)
        fname = f.name
    res = subprocess.run([str(compiler), "-I", str(bsp_dir), fname], capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[FAIL] {name}: compilation failed!\n{res.stderr}")
        sys.exit(1)
    else:
        print(f"[PASS] {name} peripheral driver compiled cleanly")

print(f"\nAll {len(modules)} RP2040 peripheral test suites passed successfully!")
