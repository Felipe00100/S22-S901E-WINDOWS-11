DefinitionBlock ("SSDT.aml", "SSDT", 2, "SAMSUN", "R0Q    ", 6)
{
    External (\_SB.PM01, UnknownObj)

    Scope (\_SB)
    {
        Name (PSUB, "MTP08450")

        Device (BTNS)
        {
            Alias (\_SB.PSUB, _SUB)

            Name (_HID, "ACPI0011")
            Name (_UID, 0)

            Name (_CRS, ResourceTemplate ()
            {
                GpioInt (Edge, ActiveBoth, ExclusiveAndWake, PullDown, 0x0000, "\\_SB.PM01", ,) { 0x07 }
                GpioInt (Edge, ActiveBoth, Exclusive,        PullUp,   0x0BB8, "\\_SB.PM01", ,) { 0xC6 }
                GpioInt (Edge, ActiveBoth, Exclusive,        PullDown, 0x0000, "\\_SB.PM01", ,) { 0x06 }
            })

            Name (_DSD, Package ()
            {
                ToUUID ("FA6BD625-9CE8-470D-A2C7-B3CA36C4282E"),

                Package ()
                {
                    Package () { 0, 1, 0, 1, 13 },
                    Package () { 1, 0, 1, 1, 129 },
                    Package () { 1, 1, 1, 12, 233 },
                    Package () { 1, 2, 1, 12, 234 }
                }
            })

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        //
        // STMicroelectronics FTS2BA61Y Touchscreen (Galaxy S22 SM-S901E / r0q)
        // SM8450 TLMM SPI bit-bang (MISO=28, MOSI=29, SCK=30, CS=31) + IRQ (GPIO 46)
        //
        Device (TSCR)
        {
            Name (_HID, "STMT0001")
            Name (_UID, 0)

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x0F11C000, 0x00004000) // TLMM GPIO 28..31
                Memory32Fixed (ReadWrite, 0x0F12E000, 0x00001000) // TLMM GPIO 46 (IRQ)
            })

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        //
        // Maxim MAX77705 Fuel Gauge (0x36, 2 mOhm) & Charger (0x69) on I2C5
        //
        Device (BATT)
        {
            Name (_HID, "MAXM7705")
            Name (_UID, 0)

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x0F1CE000, 0x00002000) // TLMM GPIO 206 (SDA), 207 (SCL)
            })

            Method (_STA, 0, NotSerialized) { Return (0x00) } // Isolated while testing URS0
        }

        //
        // Maxim MAX77705 USB-C / USB 3.0 SuperSpeed / Dual-Role / USB-PD / DP Alt-Mode Controller
        // Maps TLMM I2C5 + SM8450 DWC3 QSCRATCH + SM8450 QMP USB3/DP Combo PHY
        //
        Device (USBC)
        {
            Name (_HID, "MAXM7706")
            Name (_UID, 0)

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x0A6F8800, 0x00000400) // Region 0: DWC3 QSCRATCH (VBUS override)
                Memory32Fixed (ReadWrite, 0x088E8000, 0x00003000) // Region 1: QMP USB3/DP Combo PHY (CC1/CC2 + 4nm tuning)
            })

            Method (_STA, 0, NotSerialized) { Return (0x00) } // Disabled during bring-up
        }

        //
        // SM8450 Synopsys DWC3 USB 3.0 Dual-Role Controller (aligned to Samsung b4q DSDT)
        // Enables USB OTG Host (keyboard/mouse/hubs) AND USB Device Mode (MTP + USB-NCM/SSH)
        //
        Device (URS0)
        {
            Alias (\_SB.PSUB, _SUB)

            Name (_HID, "QCOM0C8B")
            Name (_CID, Package (0x02)
            {
                "PNP0CA1",
                "QCOMFFE1"
            })
            Name (_UID, 0)
            Name (_CCA, 0)

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x0A600000, 0x00100000) // SM8450 USB_1 DWC3 Core (1 MB page-aligned)
            })

            Method (_STA, 0, NotSerialized) { Return (0x0F) } // Enabled: urssynopsys.sys + qcursext.inf (RoleSwitchMode=1 Host) + CE2EE385 _DSM

            Device (USB0)
            {
                Name (_ADR, 0)
                Name (_S0W, 0x03)

                Name (_CRS, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , )        { 0xA5 } // GIC SPI 0x85 + 32 = 0xA5
                    Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake, , , ) { 0xA2 } // PWR EVENT IRQ
                    Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake, , , ) { 0xA3 } // HS PHY IRQ
                    Interrupt (ResourceConsumer, Edge,  ActiveHigh, SharedAndWake, , , ) { 0x20E }
                    Interrupt (ResourceConsumer, Edge,  ActiveHigh, SharedAndWake, , , ) { 0x20F }
                    Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake, , , ) { 0x211 }
                })

                Method (CCVL, 0, NotSerialized) { Return (Buffer () { 0x02 }) }
                Method (_STA, 0, NotSerialized) { Return (0x0F) }

                Method (_DSM, 4, Serialized)
                {
                    Switch (ToBuffer (Arg0))
                    {
                        Case (ToUUID ("CE2EE385-00E6-48CB-9F05-2EDB927C4899"))
                        {
                            Switch (ToInteger (Arg2))
                            {
                                Case (0)
                                {
                                    Switch (ToInteger (Arg1))
                                    {
                                        Case (0) { Return (Buffer () { 0x1D }) }
                                        Default  { Return (Buffer () { 0x01 }) }
                                    }
                                }
                                Case (2) { Return (0x00) }
                                Case (3) { Return (0x00) }
                                Case (4) { Return (0x02) }
                                Default  { Return (Buffer () { 0x00 }) }
                            }
                        }
                        Default { Return (Buffer () { 0x00 }) }
                    }
                }

                Device (RHUB)
                {
                    Name (_ADR, 0)

                    Device (PRT1)
                    {
                        Name (_ADR, 1)
                        Name (_UPC, Package ()
                        {
                            0x01,
                            0x09,
                            0x00,
                            0x00
                        })

                        Name (_PLD, Package ()
                        {
                            Buffer ()
                            {
                                0x82, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00,
                                0x69, 0x0C, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00,
                                0xFF, 0xFF, 0xFF, 0xFF
                            }
                        })
                    }
                }
            }

            Device (UFN0)
            {
                Name (_ADR, 0)
                Name (_S0W, 0x03)

                Name (_CRS, ResourceTemplate ()
                {
                    Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , )        { 0xA5 } // GIC SPI 0x85 + 32 = 0xA5
                    Interrupt (ResourceConsumer, Level, ActiveHigh, SharedAndWake, , , ) { 0xA2 } // PWR EVENT IRQ
                })

                Method (CCVL, 0, NotSerialized) { Return (Buffer () { 0x02 }) }
                Method (_STA, 0, NotSerialized) { Return (0x0F) }

                Method (_DSM, 4, Serialized)
                {
                    Switch (ToBuffer (Arg0))
                    {
                        Case (ToUUID ("FE56CFEB-49D5-4378-A8A2-2978DBE54AD2"))
                        {
                            Switch (ToInteger (Arg2))
                            {
                                Case (0)
                                {
                                    Switch (ToInteger (Arg1))
                                    {
                                        Case (0) { Return (Buffer () { 0x03 }) }
                                        Default  { Return (Buffer () { 0x01 }) }
                                    }
                                }
                                Case (1) { Return (0x20) }
                                Default  { Return (Buffer () { 0x00 }) }
                            }
                        }
                        Case (ToUUID ("18DE299F-9476-4FC9-B43B-8AEB713ED751"))
                        {
                            Switch (ToInteger (Arg2))
                            {
                                Case (0)
                                {
                                    Switch (ToInteger (Arg1))
                                    {
                                        Case (0) { Return (Buffer () { 0x03 }) }
                                        Default  { Return (Buffer () { 0x01 }) }
                                    }
                                }
                                Case (1) { Return (0x39) }
                                Default  { Return (Buffer () { 0x00 }) }
                            }
                        }
                        Default { Return (Buffer () { 0x00 }) }
                    }
                }

                Device (RHUB)
                {
                    Name (_ADR, 0)

                    Device (PRT1)
                    {
                        Name (_ADR, 1)
                        Name (_UPC, Package ()
                        {
                            0x01,
                            0x09,
                            0x00,
                            0x00
                        })

                        Name (_PLD, Package ()
                        {
                            Buffer ()
                            {
                                0x82, 0x00, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00,
                                0x69, 0x0C, 0x00, 0x00,
                                0x00, 0x00, 0x00, 0x00,
                                0xFF, 0xFF, 0xFF, 0xFF
                            }
                        })
                    }
                }
            }
        }

        //
        // Direct SM8450 USB 3.0 xHCI Host Controller (bypasses qcursext.inf RoleSwitchMode=1 Device lock)
        // Loads inbox Microsoft usbxhci.sys unconditionally in Host mode for OTG keyboard/mouse/hubs/Ethernet
        //
        Device (XHC0)
        {
            Name (_HID, "PNP0D10")
            Name (_UID, 0)
            Name (_CCA, 0)
            Name (_S0W, 0x03)

            Name (_CRS, ResourceTemplate ()
            {
                Memory32Fixed (ReadWrite, 0x0A600000, 0x00100000) // SM8450 USB_1 DWC3 xHCI Core
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 0xA5 } // GIC SPI 0x85 + 32 = 0xA5
            })

            Method (_STA, 0, NotSerialized) { Return (0x00) } // Disabled: URS0 (urssynopsys.sys) owns 0x0A600000 and spawns USB0 with CE2EE385 _DSM
        }

        //
        // Galaxy S22 (r0q) PCIe0 DesignWare iMSI-RX -> GICD MSI Bridge (s22wlanpdc.sys)
        // Claims SM8450 PCIe0 msi0..msi7 chained SPIs (GIC_SPI 141..148 + 32 = 0xAD..0xB4)
        //
        Device (WPDC)
        {
            Name (_HID, "S22W0001")
            Name (_UID, 0)

            Name (_CRS, ResourceTemplate ()
            {
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 0xAD } // msi0 (Bank 0)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 0xAE } // msi1 (Bank 1)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 0xAF } // msi2 (Bank 2: INTID 64..95)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 0xB0 } // msi3 (Bank 3)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 0xB1 } // msi4 (Bank 4)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 0xB2 } // msi5 (Bank 5)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 0xB3 } // msi6 (Bank 6)
                Interrupt (ResourceConsumer, Level, ActiveHigh, Shared, , , ) { 0xB4 } // msi7 (Bank 7)
            })

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }

        //
        // SM8450 PCIe0 Root Complex (WCN6855 / FastConnect 6900 Wi-Fi 6E: PCI\VEN_17CB&DEV_1103)
        // Addresses & INTx GSIVs verified from sm8450-samsung-r0q.dtb (SPI 0x95..0x98 + 32 = 0xB5..0xB8)
        //
        Device (PCI0)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (_CID, EisaId ("PNP0A03"))
            Name (_UID, 0)
            Name (_SEG, 0)
            Name (_BBN, 0)
            Name (_CCA, 0) // Match r0q DTS: no dma-coherent on SM8450 PCIe0

            Name (_PRT, Package (0x04)
            {
                Package (0x04) { 0xFFFF, 0, 0, 0xB5 },
                Package (0x04) { 0xFFFF, 1, 0, 0xB6 },
                Package (0x04) { 0xFFFF, 2, 0, 0xB7 },
                Package (0x04) { 0xFFFF, 3, 0, 0xB8 }
            })

            Name (_CRS, ResourceTemplate ()
            {
                WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                    0x0000, 0x0000, 0x0001, 0x0000, 0x0002, , , )
                DWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, NonCacheable, ReadWrite,
                    0x00000000, 0x60300000, 0x63FFFFFF, 0x00000000, 0x03D00000,
                    , , , AddressRangeMemory, TypeStatic)
            })

            Method (_OSC, 4, Serialized)
            {
                CreateDWordField (Arg3, 0, CDW1)
                If (LEqual (Arg0, ToUUID ("33DB4D5B-1FF7-401C-9657-7441C03DD766")))
                {
                    CreateDWordField (Arg3, 4, CDW2)
                    CreateDWordField (Arg3, 8, CDW3)
                    If (LNotEqual (Arg1, One))
                    {
                        Or (CDW1, 0x08, CDW1)
                    }
                    // Test A: restore Stage 3b _OSC (0x1D) to isolate 0xD1 regression
                    And (CDW3, 0x1D, CDW3)
                    Return (Arg3)
                }
                Or (CDW1, 0x04, CDW1)
                Return (Arg3)
            }

            Method (_STA, 0, NotSerialized) { Return (0x0F) }
        }
    }
}
