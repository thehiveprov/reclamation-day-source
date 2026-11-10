import { AfterViewInit, Directive, ElementRef } from '@angular/core';

/** Focuses the element once shown (the autofocus attribute only works on page load). */
@Directive({
  selector: '[appAutofocus]',
})
export class AutofocusDirective implements AfterViewInit {
  constructor(private readonly element: ElementRef<HTMLElement>) {}

  ngAfterViewInit(): void {
    setTimeout(() => this.element.nativeElement.focus());
  }
}
