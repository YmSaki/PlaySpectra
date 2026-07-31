package verify

import (
	"fmt"
	"io"
)

type Check struct {
	Name   string `json:"name"`
	OK     bool   `json:"ok"`
	Detail any    `json:"detail,omitempty"`
}

type Report struct {
	Name   string  `json:"name"`
	Checks []Check `json:"checks"`
}

func NewReport(name string) Report { return Report{Name: name, Checks: []Check{}} }

func (r *Report) Add(name string, condition bool, detail any) bool {
	r.Checks = append(r.Checks, Check{Name: name, OK: condition, Detail: detail})
	return condition
}

func (r Report) Passed() int {
	passed := 0
	for _, check := range r.Checks {
		if check.OK {
			passed++
		}
	}
	return passed
}

func (r Report) OK() bool { return r.Passed() == len(r.Checks) }

func (r Report) ExitCode() int {
	if r.OK() {
		return 0
	}
	return 1
}

func (r Report) WriteText(output io.Writer) {
	for _, check := range r.Checks {
		status := "FAIL"
		if check.OK {
			status = "PASS"
		}
		detail := ""
		if check.Detail != nil && fmt.Sprint(check.Detail) != "" {
			detail = " - " + fmt.Sprint(check.Detail)
		}
		fmt.Fprintf(output, "%s %s%s\n", status, check.Name, detail)
	}
	fmt.Fprintf(output, "\n=== %d/%d PASS ===\n", r.Passed(), len(r.Checks))
}
