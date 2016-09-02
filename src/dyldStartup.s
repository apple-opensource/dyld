/*
 * Copyright (c) 1999-2011 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * C runtime startup for interface to the dynamic linker.
 * This is the same as the entry point in crt0.o with the addition of the
 * address of the mach header passed as the an extra first argument.
 *
 * Kernel sets up stack frame to look like:
 *
 *	| STRING AREA |
 *	+-------------+
 *	|      0      |	
*	+-------------+
 *	|  apple[n]   |
 *	+-------------+
 *	       :
 *	+-------------+
 *	|  apple[0]   | 
 *	+-------------+ 
 *	|      0      |
 *	+-------------+
 *	|    env[n]   |
 *	+-------------+
 *	       :
 *	       :
 *	+-------------+
 *	|    env[0]   |
 *	+-------------+
 *	|      0      |
 *	+-------------+
 *	| arg[argc-1] |
 *	+-------------+
 *	       :
 *	       :
 *	+-------------+
 *	|    arg[0]   |
 *	+-------------+
 *	|     argc    |
 *	+-------------+
 * sp->	|      mh     | address of where the a.out's file offset 0 is in memory
 *	+-------------+
 *
 *	Where arg[i] and env[i] point into the STRING AREA
 */



	// Hack to make _offset_to_dyld_all_image_infos work
	// Without this local symbol, assembler will error out about in subtraction expression
	// The real _dyld_all_image_infos (non-weak) _dyld_all_image_infos is defined in dyld_gdb.o
	// and the linker with throw this one away and use the real one instead.
	.section __DATA,__datacoal_nt,coalesced
	.globl _dyld_all_image_infos
	.weak_definition _dyld_all_image_infos
_dyld_all_image_infos:	.long 0



	.globl __dyld_start

#ifdef __i386__
	.data
__dyld_start_static_picbase: 
	.long   L__dyld_start_picbase
Lmh:	.long	___dso_handle

	.text
	.align 2
# stable entry points into dyld
	.globl	_stub_binding_helper
_stub_binding_helper:
	jmp	_stub_binding_helper_interface
	nop
	nop
	nop
	.globl	_dyld_func_lookup
_dyld_func_lookup:
	jmp	__Z18lookupDyldFunctionPKcPm
	nop
	nop
	nop
_offset_to_dyld_all_image_infos:
	.long	_dyld_all_image_infos - . + 0x1010 
	.long	0
	# space for future stable entry points
	.space	16

	
	
	.text
	.align	4, 0x90
	.globl __dyld_start
__dyld_start:
	popl	%edx		# edx = mh of app
	pushl	$0		# push a zero for debugger end of frames marker
	movl	%esp,%ebp	# pointer to base of kernel frame
	andl    $-16,%esp       # force SSE alignment
	subl	$32,%esp	# room for locals and outgoing parameters
	
	call    L__dyld_start_picbase
L__dyld_start_picbase:	
	popl	%ebx		# set %ebx to runtime value of picbase

   	movl	Lmh-L__dyld_start_picbase(%ebx), %ecx # ecx = prefered load address
   	movl	__dyld_start_static_picbase-L__dyld_start_picbase(%ebx), %eax
	subl    %eax, %ebx      # ebx = slide = L__dyld_start_picbase - [__dyld_start_static_picbase]
	addl	%ebx, %ecx	# ecx = actual load address
	# call dyldbootstrap::start(app_mh, argc, argv, slide, dyld_mh, &startGlue)
	movl	%edx,(%esp)	# param1 = app_mh
	movl	4(%ebp),%eax	
	movl	%eax,4(%esp)	# param2 = argc
	lea     8(%ebp),%eax	
	movl	%eax,8(%esp)	# param3 = argv
	movl	%ebx,12(%esp)	# param4 = slide
	movl	%ecx,16(%esp)	# param5 = actual load address
	lea	28(%esp),%eax
	movl	%eax,20(%esp)	# param6 = &startGlue
	call	__ZN13dyldbootstrap5startEPK12macho_headeriPPKclS2_Pm	
	movl	28(%esp),%edx
	cmpl	$0,%edx
	jne	Lnew

    	# clean up stack and jump to "start" in main executable
	movl	%ebp,%esp	# restore the unaligned stack pointer
	addl	$4,%esp		# remove debugger end frame marker
	movl	$0,%ebp		# restore ebp back to zero
	jmp	*%eax		# jump to the entry point

	# LC_MAIN case, set up stack for call to main() 
Lnew:	movl	4(%ebp),%ebx
	movl	%ebx,(%esp)	# main param1 = argc
	leal	8(%ebp),%ecx
	movl	%ecx,4(%esp)	# main param2 = argv
	leal	0x4(%ecx,%ebx,4),%ebx
	movl	%ebx,8(%esp)	# main param3 = env
Lapple:	movl	(%ebx),%ecx	# look for NULL ending env[] array
	add	$4,%ebx
	testl	%ecx,%ecx
	jne	Lapple		# once found, next pointer is "apple" parameter now in %ebx
	movl	%ebx,12(%esp)	# main param4 = apple
	pushl	%edx		# simulate return address into _start in libdyld
	jmp	*%eax		# jump to main(argc,argv,env,apple) with return address set to _start
	
	
	.globl dyld_stub_binding_helper
dyld_stub_binding_helper:
	hlt
L_end:
#endif /* __i386__ */



#if __x86_64__
	.data
	.align 3
__dyld_start_static: 
	.quad   __dyld_start

# stable entry points into dyld
	.text
	.align 2
	.globl	_stub_binding_helper
_stub_binding_helper:
	jmp	_stub_binding_helper_interface
	nop
	nop
	nop
	.globl	_dyld_func_lookup
_dyld_func_lookup:
	jmp	__Z18lookupDyldFunctionPKcPm
	nop
	nop
	nop
_offset_to_dyld_all_image_infos:
	.long	_dyld_all_image_infos - . + 0x1010 
	.long	0
	# space for future stable entry points
	.space	16


	.text
	.align 2,0x90
	.globl __dyld_start
__dyld_start:
	popq	%rdi		# param1 = mh of app
	pushq	$0		# push a zero for debugger end of frames marker
	movq	%rsp,%rbp	# pointer to base of kernel frame
	andq    $-16,%rsp       # force SSE alignment
	subq	$16,%rsp	# room for local variables
	
	# call dyldbootstrap::start(app_mh, argc, argv, slide, dyld_mh, &startGlue)
	movl	8(%rbp),%esi	# param2 = argc into %esi
	leaq	16(%rbp),%rdx	# param3 = &argv[0] into %rdx
	movq	__dyld_start_static(%rip), %r8
	leaq	__dyld_start(%rip), %rcx
	subq	 %r8, %rcx	# param4 = slide into %rcx
	leaq	___dso_handle(%rip),%r8 # param5 = dyldsMachHeader
	leaq	-8(%rbp),%r9
	call	__ZN13dyldbootstrap5startEPK12macho_headeriPPKclS2_Pm
	movq	-8(%rbp),%rdi
	cmpq	$0,%rdi
	jne	Lnew

    	# clean up stack and jump to "start" in main executable
	movq	%rbp,%rsp	# restore the unaligned stack pointer
	addq	$8,%rsp 	# remove the mh argument, and debugger end frame marker
	movq	$0,%rbp		# restore ebp back to zero
	jmp	*%rax		# jump to the entry point
	
	# LC_MAIN case, set up stack for call to main() 
Lnew:	addq	$16,%rsp	# remove local variables
	pushq	%rdi		# simulate return address into _start in libdyld
	movq	8(%rbp),%rdi	# main param1 = argc into %rdi
	leaq	16(%rbp),%rsi	# main param2 = &argv[0] into %rsi
	leaq	0x8(%rsi,%rdi,8),%rdx # main param3 = &env[0] into %rdx
 	movq	%rdx,%rcx
Lapple: movq	(%rcx),%r8
	add	$8,%rcx
	testq	%r8,%r8		# look for NULL ending env[] array
	jne	Lapple		# main param4 = apple into %rcx
	jmp	*%rax		# jump to main(argc,argv,env,apple) with return address set to _start
	
#endif /* __x86_64__ */



#if __arm__
	.syntax unified
	.data
	.align 2
__dyld_start_static_picbase: 
	.long	L__dyld_start_picbase

	.text
	.align 2
	.globl	_stub_binding_helper
_stub_binding_helper:
	b	_stub_binding_helper_interface
	nop 
	
	.globl	_dyld_func_lookup
_dyld_func_lookup:
	b       _branch_to_lookupDyldFunction
	nop
	
_offset_to_dyld_all_image_infos:
	.long	_dyld_all_image_infos - . + 0x1010 
	.long	0
	# space for future stable entry points
	.space	16
    
    
	// Hack to make ___dso_handle work
	// Without this local symbol, assembler will error out about in subtraction expression
	// The real ___dso_handle (non-weak) sythesized by the linker
	// Since this one is weak, the linker will throw this one away and use the real one instead.
	.data
	.globl ___dso_handle
	.weak_definition ___dso_handle
___dso_handle:	.long 0

	.text
	.align 2
__dyld_start:
	mov	r8, sp		// save stack pointer
	sub	sp, #16		// make room for outgoing parameters
	bic     sp, sp, #7	// force 8-byte alignment

	// call dyldbootstrap::start(app_mh, argc, argv, slide, dyld_mh, &startGlue)

	ldr	r3, L__dyld_start_picbase_ptr
L__dyld_start_picbase:
	sub	r0, pc, #8	// load actual PC
	ldr	r3, [r0, r3]	// load expected PC
	sub	r3, r0, r3	// r3 = slide

	ldr	r0, [r8]	// r0 = mach_header
	ldr	r1, [r8, #4]	// r1 = argc
	add	r2, r8, #8	// r2 = argv

	ldr	r4, Lmh
L3:	add	r4, r4, pc	
	str	r4, [sp, #0]	// [sp] = dyld_mh
	add	r4, sp, #12
	str	r4, [sp, #4]	// [sp+4] = &startGlue
       
	bl	__ZN13dyldbootstrap5startEPK12macho_headeriPPKclS2_Pm
	ldr	r5, [sp, #12]
	cmp	r5, #0
	bne	Lnew
	
	// traditional case, clean up stack and jump to result
	add	sp, r8, #4	// remove the mach_header argument.
	bx	r0		// jump to the program's entry point

	// LC_MAIN case, set up stack for call to main()
Lnew:	mov	lr, r5		    // simulate return address into _start in libdyld
	mov	r5, r0		    // save address of main() for later use
	ldr	r0, [r8, #4]	    // main param1 = argc
	add	r1, r8, #8	    // main param2 = argv
	add	r2, r1, r0, lsl #2  
	add	r2, r2, #4	    // main param3 = &env[0]
	mov	r3, r2
Lapple:	ldr	r4, [r3]
	add	r3, #4
	cmp	r4, #0
	bne	Lapple		    // main param4 = apple
	bx	r5

	.align 2
L__dyld_start_picbase_ptr:
	.long	__dyld_start_static_picbase-L__dyld_start_picbase
Lmh:	.long   ___dso_handle-L3-8
	
	.text
	.align 2
_branch_to_lookupDyldFunction:
	// arm has no "bx label" instruction, so need this island in case lookupDyldFunction() is in thumb
	ldr ip, L2
L1:	ldr pc, [pc, ip]
L2:	.long   _lookupDyldFunction_ptr-8-L1
       
 	.data
	.align 2
_lookupDyldFunction_ptr:
	.long	__Z18lookupDyldFunctionPKcPm
	
      
	.text
	.globl dyld_stub_binding_helper
dyld_stub_binding_helper:
	trap

L_end:
#endif /* __arm__ */

/*
 * dyld calls this function to terminate a process.
 * It has a label so that CrashReporter can distinguish this
 * termination from a random crash.  rdar://problem/4764143
 */
	.text
	.align 2
	.globl	_dyld_fatal_error
_dyld_fatal_error:
#if __arm__
    trap
    nop
#elif __x86_64__ || __i386__
    int3
    nop
#else
    #error unknown architecture
#endif

#if __arm__
	// work around for:  <rdar://problem/6530727> gdb-1109: notifier in dyld does not work if it is in thumb
 	.text
	.align 2
	.globl	_gdb_image_notifier
	.private_extern _gdb_image_notifier
_gdb_image_notifier:
	bx  lr
#endif




